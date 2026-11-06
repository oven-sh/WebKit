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
#include <algorithm>
#include <limits>
#include <wtf/CheckedArithmetic.h>
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

static Opcode negatedComparison(Opcode comparison)
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

enum class StepsOfLoopsInside : uint8_t { AreUnknown, GiveDirection };

static LoopInduction findInduction(FunctionAnalyses& analyses, const NaturalLoop& loop, StepsOfLoopsInside stepsOfLoopsInside)
{
    const Program& program = analyses.program();
    const Function& function = analyses.function();
    const CFG& cfg = analyses.cfg();
    const Dominators& dominators = analyses.dominators();
    const NaturalLoops& loops = analyses.naturalLoops();
    const UseLists& useLists = analyses.useLists();
    IntegerRelations relations(analyses);

    auto isInLoop = [&](BlockID block) {
        return loops.belongsTo(block, loop);
    };
    auto typeOf = [&](ValueID id) -> const Type& {
        return program.typeOf(function, id);
    };
    auto integerConstantOf = [&](ValueID id) {
        return integerConstantThroughCasts(program, function, id);
    };
    auto isNeverNaN = [&](ValueID id) {
        return typeOf(id).isNumber() && !typeOf(id).mayBeNaN();
    };
    auto numericIdentityOf = [&](ValueID id) {
        id = analyses.identityOf(id);
        for (;;) {
            const Value& value = function.at(id);
            bool isConversion = value.opcode == Opcode::ToNumber || value.opcode == Opcode::ToNumeric;
            if (!isConversion || !typeOf(value.operand(0)).isNumber())
                return id;
            id = analyses.identityOf(value.operand(0));
        }
    };

    Vector<Edge, 2> enteringEdges;
    Vector<Edge, 2> backEdges;
    for (Edge edge : cfg.incomingEdges(loop.header()))
        (isInLoop(edge.from) ? backEdges : enteringEdges).append(edge);

    auto findStart = [&](std::span<const ValueID> enteringArguments) -> ValueID {
        if (enteringArguments.empty() || std::ranges::any_of(enteringArguments, [](ValueID argument) { return !argument; }))
            return { };

        ValueID first = enteringArguments.front();
        bool isOneRunTimeValue = std::ranges::all_of(enteringArguments, [&](ValueID argument) {
            if (analyses.identities().areSameValue(first, argument))
                return true;
            std::optional<int64_t> constant = integerConstantOf(argument);
            return constant && constant == integerConstantOf(first);
        });
        if (!isOneRunTimeValue)
            return { };

        bool isOneDefinition = std::ranges::all_of(enteringArguments, [&](ValueID argument) {
            return argument == first;
        });
        return isOneDefinition || integerConstantOf(first) ? first : analyses.identityOf(first);
    };

    auto findVariable = [&](uint32_t parameterIndex) -> std::optional<InductionVariable> {
        auto argumentsOn = [&](std::span<const Edge> edges) -> std::optional<Vector<ValueID, 2>> {
            Vector<ValueID, 2> arguments;
            for (Edge edge : edges) {
                IncomingValue incoming = incomingValueOf(function, edge, parameterIndex);
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
        };
        auto enteringArguments = argumentsOn(enteringEdges.span());
        auto carriedArguments = argumentsOn(backEdges.span());
        if (!enteringArguments || !carriedArguments)
            return std::nullopt;

        InductionVariable variable;
        variable.parameter = function.at(loop.header()).parameters[parameterIndex];
        variable.parameterIndex = parameterIndex;
        variable.start = findStart(enteringArguments->span());

        struct PendingValue {
            ValueID value;
            int64_t accumulated;
        };
        Range totals;
        std::optional<InductionStep> invariant;
        bool isUnknown = false;
        bool mayStepFurtherUp = false;
        bool mayStepFurtherDown = false;
        Vector<PendingValue, 8> pending;
        UncheckedKeyHashMap<ValueID, int64_t> accumulatedAtParameter;

        auto giveUp = [&] {
            isUnknown = true;
        };
        auto enqueueOperand = [&](ValueID operand, CheckedInt64 accumulated) {
            if (accumulated.hasOverflowed())
                return giveUp();
            pending.append(PendingValue { operand, accumulated.value() });
        };
        auto visitBlockParameter = [&](ValueID parameter, ValueLocation location, int64_t accumulated) {
            if (location.block == loop.header())
                return giveUp();
            auto visited = accumulatedAtParameter.add(parameter, accumulated);
            if (!visited.isNewEntry) {
                if (visited.iterator->value == accumulated)
                    return;
                // A loop inside steps the variable as well, any number of times. Where every way that comes by again has added, the least sum is the one without them.
                if (stepsOfLoopsInside == StepsOfLoopsInside::AreUnknown)
                    return giveUp();
                (accumulated > visited.iterator->value ? mayStepFurtherUp : mayStepFurtherDown) = true;
                return;
            }
            for (Edge edge : cfg.incomingEdges(location.block)) {
                ValueID incoming = valuePassedOn(function, edge, location.index);
                if (!incoming)
                    return giveUp();
                pending.append(PendingValue { incoming, accumulated });
            }
        };
        auto visitInvariantStep = [&](const Value& sum, int64_t accumulated) {
            auto isStep = [&](ValueID stepped, ValueID amount) {
                return numericIdentityOf(stepped) == variable.parameter && isInvariantIn(analyses, loop, amount);
            };
            ValueID amount;
            if (isStep(sum.operand(0), sum.operand(1)))
                amount = sum.operand(1);
            else if (sum.opcode == Opcode::Add && isStep(sum.operand(1), sum.operand(0)))
                amount = sum.operand(0);
            if (!amount || accumulated)
                return giveUp();
            InductionStep step { InductionStepKind::Invariant, 0, analyses.identityOf(amount), sum.opcode == Opcode::Sub };
            if (!invariant)
                invariant = step;
            else if (*invariant != step)
                giveUp();
        };
        auto visitOperation = [&](const Value& value, int64_t accumulated) {
            switch (value.opcode) {
            case Opcode::Increment:
                return enqueueOperand(value.operand(0), CheckedInt64(accumulated) + int64_t { 1 });
            case Opcode::Decrement:
                return enqueueOperand(value.operand(0), CheckedInt64(accumulated) - int64_t { 1 });
            case Opcode::Add:
                if (std::optional<int64_t> constant = integerConstantOf(value.operand(1)))
                    return enqueueOperand(value.operand(0), CheckedInt64(accumulated) + *constant);
                if (std::optional<int64_t> constant = integerConstantOf(value.operand(0)))
                    return enqueueOperand(value.operand(1), CheckedInt64(accumulated) + *constant);
                return visitInvariantStep(value, accumulated);
            case Opcode::Sub:
                if (std::optional<int64_t> constant = integerConstantOf(value.operand(1)))
                    return enqueueOperand(value.operand(0), CheckedInt64(accumulated) - *constant);
                return visitInvariantStep(value, accumulated);
            default:
                return giveUp();
            }
        };
        auto visit = [&](PendingValue next) {
            ValueID id = numericIdentityOf(next.value);
            if (id == variable.parameter) {
                totals = totals.unionWith(Range::one(next.accumulated));
                return;
            }
            ValueLocation location = useLists.locationOf(id);
            if (!isInLoop(location.block))
                return giveUp();
            switch (location.placement) {
            case ValuePlacement::Detached:
                return giveUp();
            case ValuePlacement::Parameter:
                return visitBlockParameter(id, location, next.accumulated);
            case ValuePlacement::InBlock:
                return visitOperation(function.at(id), next.accumulated);
            }
            RELEASE_ASSERT_NOT_REACHED();
        };

        for (ValueID argument : *carriedArguments) {
            if (!argument)
                return std::nullopt;
            ValueID stepped = analyses.identityOf(argument);
            if (variable.steppedValues.contains(stepped))
                continue;
            variable.steppedValues.append(stepped);
            accumulatedAtParameter.clear();
            pending.append(PendingValue { stepped, 0 });
            while (!pending.isEmpty() && !isUnknown)
                visit(pending.takeLast());
            if (isUnknown)
                return std::nullopt;
        }

        if (mayStepFurtherUp || mayStepFurtherDown) {
            if (invariant || totals.isEmpty() || (mayStepFurtherUp && mayStepFurtherDown))
                return std::nullopt;
            totals = totals.unionWith(Range::one(mayStepFurtherUp ? std::numeric_limits<int64_t>::max() : std::numeric_limits<int64_t>::min()));
        }

        if (!invariant) {
            if (totals.isEmpty() || totals == Range::one(0))
                return std::nullopt;
            if (totals.isSingleValue())
                variable.step = { InductionStepKind::Constant, totals.minimum, ValueID(), false };
            variable.direction = directionOfAmounts(totals);
            return variable;
        }

        if (totals.isEmpty())
            variable.step = *invariant;
        const Type& type = typeOf(invariant->invariant);
        InductionDirection direction = type.isInteger() ? directionOfAmounts(type.integers) : InductionDirection::Unknown;
        if (invariant->isSubtracted)
            direction = oppositeOf(direction);
        if (totals.isEmpty() || directionOfAmounts(totals) == direction)
            variable.direction = direction;
        return variable;
    };

    LoopInduction induction { .header = loop.header(), .loopIndex = loop.index(), .variables = { } };
    Vector<InductionVariable, 2>& variables = induction.variables;
    size_t numberOfParameters = function.at(loop.header()).parameters.size();
    for (uint32_t parameterIndex = 0; parameterIndex < numberOfParameters; ++parameterIndex) {
        if (std::optional<InductionVariable> variable = findVariable(parameterIndex))
            variables.append(WTF::move(*variable));
    }
    if (variables.isEmpty())
        return induction;

    auto findTestedVariable = [&](ValueID id) -> std::pair<InductionVariable*, TestedValue> {
        for (InductionVariable& variable : variables) {
            if (variable.parameter == id)
                return { &variable, TestedValue::BeforeStep };
        }
        for (InductionVariable& variable : variables) {
            if (variable.steppedValues.contains(id))
                return { &variable, TestedValue::AfterStep };
        }
        const Value& sum = function.at(id);
        if (sum.opcode != Opcode::Add)
            return { };
        for (InductionVariable& variable : variables) {
            if (variable.step.kind != InductionStepKind::Constant || !variable.step.constant)
                continue;
            for (uint32_t side = 0; side < 2; ++side) {
                if (numericIdentityOf(sum.operand(side)) == variable.parameter && integerConstantOf(sum.operand(1 - side)) == variable.step.constant)
                    return { &variable, TestedValue::AfterStep };
            }
        }
        return { };
    };
    auto isComputedInLoop = [&](ValueID bound) {
        ValueID definition = analyses.identityOf(bound);
        ValueLocation location = useLists.locationOf(definition);
        if (location.placement != ValuePlacement::InBlock || !isInLoop(location.block))
            return false;
        if (!analyses.isUnobservable(definition))
            return false;

        bool hasInvariantOperands = true;
        function.at(definition).forEachOperand([&](ValueID operand, uint32_t) {
            hasInvariantOperands &= isInvariantIn(analyses, loop, operand);
        });
        return hasInvariantOperands;
    };
    auto proveRelation = [&](Opcode relation, ValueID left, ValueID right, BlockID at) {
        switch (relation) {
        case Opcode::Less:
            return relations.isAtMost(left, right, -1, at);
        case Opcode::LessEq:
            return relations.isAtMost(left, right, 0, at);
        case Opcode::Greater:
            return relations.isAtMost(right, left, -1, at);
        case Opcode::GreaterEq:
            return relations.isAtMost(right, left, 0, at);
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    };
    auto entryRelationOf = [&](ValueID start, const InductionTest& test) {
        if (!start || test.boundKind != LoopBoundKind::Invariant)
            return EntryRelation::Unknown;

        ValueID bound = isInLoop(useLists.blockOf(test.bound)) ? analyses.identityOf(test.bound) : test.bound;
        if (!isNeverNaN(start) || !isNeverNaN(bound))
            return EntryRelation::Unknown;

        bool holdsOnEveryEdge = std::ranges::all_of(enteringEdges, [&](Edge edge) {
            return proveRelation(test.relation, start, bound, edge.from) == IntegerRelations::Answer::Proven;
        });
        return holdsOnEveryEdge ? EntryRelation::Holds : EntryRelation::Unknown;
    };
    auto isOnEveryTrip = [&](BlockID testingBlock) {
        return std::ranges::all_of(backEdges, [&](Edge edge) {
            if (edge.from == testingBlock)
                return !edge.isHandlerEdge();
            return dominators.terminatorDominates(testingBlock, edge.from);
        });
    };

    Vector<BlockID, 4> testingBlocks;
    for (unsigned i = 0; i < loop.size(); ++i) {
        BlockID block = loop.at(i);
        const Terminator& terminator = function.at(block).terminator;
        if (!terminator.isConditionalBranch())
            continue;
        if (isInLoop(terminator.taken().block) == isInLoop(terminator.notTaken().block))
            continue;
        if (relationalOperators.contains(function.at(terminator.conditionOperand()).opcode))
            testingBlocks.append(block);
    }
    AOT::sortInLine(testingBlocks, { }, [&](BlockID block) {
        return cfg.reversePostOrderIndex(block);
    });

    for (BlockID block : testingBlocks) {
        const Terminator& terminator = function.at(block).terminator;
        const Value& comparison = function.at(terminator.conditionOperand());
        bool staysOnTaken = isInLoop(terminator.taken().block);
        if (!staysOnTaken && (!isNeverNaN(comparison.operand(0)) || !isNeverNaN(comparison.operand(1))))
            continue;

        std::optional<LoopExits> exits;
        for (uint32_t side = 0; side < 2; ++side) {
            auto [variable, tested] = findTestedVariable(numericIdentityOf(comparison.operand(side)));
            if (!variable)
                continue;
            if (!exits)
                exits = isOnEveryTrip(block) ? exitsOf(analyses, loop, block) : LoopExits::Unknown;

            Opcode relation = side ? mirroredComparison(comparison.opcode) : comparison.opcode;
            ValueID bound = comparison.operand(1 - side);
            LoopBoundKind boundKind = isInvariantIn(analyses, loop, bound) ? LoopBoundKind::Invariant : isComputedInLoop(bound) ? LoopBoundKind::ComputedInLoop : LoopBoundKind::Unknown;
            InductionTest test {
                .block = block,
                .comparison = terminator.conditionOperand(),
                .relation = staysOnTaken ? relation : negatedComparison(relation),
                .tested = tested,
                .bound = bound,
                .boundKind = boundKind,
                .stayingEdge = Edge { block, staysOnTaken ? 0u : 1u },
                .entryRelation = EntryRelation::Unknown,
                .exits = *exits,
            };
            test.entryRelation = entryRelationOf(variable->start, test);
            variable->tests.append(test);
        }
    }
    return induction;
}

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
        m_loops.append(findInduction(analyses, loops.loop(index), StepsOfLoopsInside::AreUnknown));
}

LoopInduction findInductionThroughLoopsInside(FunctionAnalyses& analyses, const NaturalLoop& loop)
{
    return findInduction(analyses, loop, StepsOfLoopsInside::GiveDirection);
}

const LoopInduction& InductionVariables::of(const NaturalLoop& loop) const
{
    return m_loops[loop.index()];
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
