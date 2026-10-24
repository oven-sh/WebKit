/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRewrite.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTDecidedBranches.h"
#include "AOTInt32Conversions.h"
#include "AOTSort.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <limits>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

// ReduceIntegerDivisions.

static bool isExactInteger(const Type& type, int64_t limit)
{
    return type.isInteger() && type.integers.isSubsetOf({ -limit, limit });
}

static bool canDivideAsIntegers(const BuildContext& build, QuotientRounding rounding, const Type& dividend, const Type& divisor)
{
    int64_t limit = rounding == QuotientRounding::Round ? maxExactInteger >> 2 : maxExactInteger;
    if (!isExactInteger(dividend, limit) || !isExactInteger(divisor, limit))
        return false;
    Range dividends = dividend.integers;
    Range divisors = divisor.integers;
    if (dividends.minimum == dividends.maximum && divisors.minimum == divisors.maximum)
        return false;
    if (rounding == QuotientRounding::ToInt32)
        return true;
    if (rounding == QuotientRounding::Floor)
        return divisors.minimum > 0 || build.isSabotaged(Sabotage::FlooredQuotientsHaveAnyDivisor);
    return (divisors.minimum > 0 && dividends.minimum >= 0) || build.isSabotaged(Sabotage::RoundedQuotientsHaveAnySign);
}

static std::optional<Value> findIntegerDivisionFor(Rewrite& rewrite, ValueID operand, QuotientRounding rounding)
{
    const Value& quotient = rewrite.at(sameObjectThroughCasts(rewrite.function, operand));
    if (quotient.opcode != Opcode::Div || !canDivideAsIntegers(rewrite.context.build, rounding, rewrite.typeOf(quotient.leftOperand()), rewrite.typeOf(quotient.rightOperand())))
        return std::nullopt;
    Value division = IRBuilder::make(Opcode::DivideIntegers, { quotient.leftOperand(), quotient.rightOperand() });
    division.setRounding(rounding);
    return division;
}

static std::optional<QuotientRounding> roundingOf(std::optional<InlineBuiltin> builtin)
{
    if (!builtin)
        return std::nullopt;
    switch (*builtin) {
    case InlineBuiltin::MathFloor:
        return QuotientRounding::Floor;
    case InlineBuiltin::MathCeil:
        return QuotientRounding::Ceil;
    case InlineBuiltin::MathTrunc:
        return QuotientRounding::Truncate;
    case InlineBuiltin::MathRound:
        return QuotientRounding::Round;
    default:
        return std::nullopt;
    }
}

static void reduceRoundedQuotient(Rewrite& rewrite, const Site& call)
{
    std::optional<QuotientRounding> rounding = roundingOf(rewrite.program.builtins().at(call.value.overload()).code.implementation.inlineBuiltin);
    if (!rounding || call.value.builtinCallForm() != BuiltinCallForm::Whole || call.value.argumentsOperands().size() != 1)
        return;
    if (std::optional<Value> division = findIntegerDivisionFor(rewrite, call.value.argumentsOperands()[0], *rounding)) {
        rewrite.later([&rewrite, id = call.id, division = WTF::move(*division)]() mutable {
            rewrite.become(id, WTF::move(division));
            rewrite.count(StatisticID::IntegerDivisionsReduced);
        });
    }
}

static void reduceQuotientConvertedToInt32(Rewrite& rewrite, const Site& user)
{
    user.value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        std::optional<Value> division = findIntegerDivisionFor(rewrite, operand, QuotientRounding::ToInt32);
        if (!division)
            return;
        rewrite.later([&rewrite, point = user.point, id = user.id, operandIndex, division = WTF::move(*division)]() mutable {
            ValueID quotient = rewrite.insertBefore(point, rewrite.at(id).origin(), WTF::move(division));
            rewrite.edit(id, ValueEdit::Definition).setOperand(operandIndex, quotient);
            rewrite.retype(quotient);
            rewrite.count(StatisticID::IntegerDivisionsReduced);
        });
    });
}

static constexpr Rule integerDivisionRules[] {
    { OpcodeSet { Opcode::CallBuiltin }, reduceRoundedQuotient },
    { int32ConvertingOperations, reduceQuotientConvertedToInt32 },
};

PassOutcome reduceIntegerDivisions(FunctionPassContext& context)
{
    return runRules<integerDivisionRules, Rounds::UntilUnchanged>(context);
}

// PropagateBranchConditions.

static bool isAnyUsedTwice(Rewrite& rewrite, const BitVector& values)
{
    BitVector isUsed(rewrite.function.values.size());
    bool isUsedTwice = false;
    auto addUse = [&](ValueID used) {
        if (used && values.quickGet(used.index()))
            isUsedTwice |= isUsed.quickSet(used.index());
    };
    for (BlockID id : rewrite.blocks()) {
        for (ValueID value : rewrite.at(id).values)
            rewrite.at(value).forEachChild(addUse);
        rewrite.at(id).forEachChildOutsideValues(addUse);
        if (isUsedTwice)
            return true;
    }
    return false;
}

static bool canUseValuesOf(Rewrite& rewrite, BlockID block, const Use& use)
{
    if (use.block != block)
        return rewrite.analyses.dominators().terminatorDominates(block, use.block);
    return use.kind != Use::Kind::HandlerArgument && use.kind != Use::Kind::BlockSubject;
}

static bool isConditionOfProvingBranch(Rewrite& rewrite, const Use& use)
{
    return use.kind == Use::Kind::TerminatorOperand && rewrite.at(use.block).terminator.isConditionalBranch()
        && (hasEdgeResults(rewrite.function, Edge { use.block, 0 }) || hasEdgeResults(rewrite.function, Edge { use.block, 1 }));
}

// Whether a value is truthy never changes, so a test that another test of the same value dominates is that test.
static PassOutcome mergeRepeatedTruthinessTests(FunctionPassContext& context)
{
    struct TruthinessTest {
        ValueID tested;
        ValueID test;
        BlockID block;
    };
    if (!context.analyses.opcodesOfValues().contains(Opcode::ToBoolean))
        return PassOutcome::Unchanged;
    Rewrite rewrite(context);
    Vector<TruthinessTest, 16> tests;
    for (BlockID block : rewrite.blocks()) {
        for (ValueID id : rewrite.at(block).values) {
            if (rewrite.at(id).opcode == Opcode::ToBoolean)
                tests.append({ rewrite.at(id).operand(0), id, block });
        }
    }
    if (tests.size() < 2)
        return PassOutcome::Unchanged;
    for (TruthinessTest& test : tests)
        test.tested = rewrite.analyses.identities().identityOf(test.tested);
    AOT::stableSort(tests, { }, &TruthinessTest::tested);

    size_t firstKept = 0;
    size_t endOfKept = 0;
    for (const TruthinessTest& test : tests) {
        if (endOfKept == firstKept || tests[firstKept].tested != test.tested)
            firstKept = endOfKept;
        auto kept = tests.span().subspan(firstKept, endOfKept - firstKept);
        auto dominating = std::ranges::find_if(kept, [&](const TruthinessTest& earlier) {
            return earlier.block == test.block || rewrite.analyses.dominators().terminatorDominates(earlier.block, test.block);
        });
        if (dominating == kept.end()) {
            tests[endOfKept++] = test;
            continue;
        }
        // What a branch proves, it proves of the operand of its condition: of a value that another test has narrowed it hands on more.
        bool testsSameValue = rewrite.at(test.test).operand(0) == rewrite.at(dominating->test).operand(0);
        for (const Use& use : rewrite.uses().usesOf(test.test)) {
            if (testsSameValue || !isConditionOfProvingBranch(rewrite, use))
                rewrite.replaceUse(use, dominating->test);
        }
    }
    return rewrite.commit();
}

// A use that only one edge of a branch on the condition leads to sees what the condition is on that edge.
static void propagateCondition(Rewrite& rewrite, ValueID condition)
{
    struct DecidingEdge {
        Edge edge;
        ValueID constant;
    };
    std::span<const Use> uses = rewrite.uses().usesOf(condition);
    if (uses.size() < 2)
        return;
    Vector<DecidingEdge, 4> edges;
    for (const Use& use : uses) {
        if (use.kind != Use::Kind::TerminatorOperand || rewrite.at(use.block).terminator.kind != TerminatorKind::Branch)
            continue;
        for (Edge edge : { Edge { use.block, 0 }, Edge { use.block, 1 } }) {
            if (rewrite.cfg().singleIncomingEdge(targetOf(rewrite.function, edge).block) == edge)
                edges.append({ edge, ValueID() });
        }
    }
    Vector<std::pair<Use, unsigned>, 4> decidedUses;
    for (const Use& use : uses) {
        if (isConditionOfProvingBranch(rewrite, use))
            continue;
        size_t index = edges.findIf([&](const DecidingEdge& deciding) { return canUseValuesOf(rewrite, targetOf(rewrite.function, deciding.edge).block, use); });
        if (index != notFound)
            decidedUses.append({ use, static_cast<unsigned>(index) });
    }
    if (decidedUses.isEmpty())
        return;
    rewrite.later([&rewrite, condition, edges = WTF::move(edges), decidedUses = WTF::move(decidedUses)]() mutable {
        for (auto& [use, index] : decidedUses) {
            auto& [edge, constant] = edges[index];
            if (!constant) {
                BlockID target = targetOf(rewrite.function, edge).block;
                Value boolean = IRBuilder::make(Opcode::Boolean);
                boolean.setBoolean(!edge.targetIndex);
                constant = rewrite.insertBefore({ target, indexAfterCaughtValues(rewrite.function, rewrite.at(target)) }, rewrite.at(condition).origin(), WTF::move(boolean));
            }
            rewrite.replaceUse(use, constant);
        }
    });
}

PassOutcome propagateBranchConditions(FunctionPassContext& context)
{
    PassOutcome outcomeOfMerging = mergeRepeatedTruthinessTests(context);
    Rewrite rewrite(context);
    BitVector isVisited;
    BitVector isUndecided(context.function.values.size());
    Vector<ValueID, 16> undecidedConditions;
    for (BlockID block : rewrite.blocks()) {
        const Terminator& terminator = rewrite.at(block).terminator;
        if (terminator.kind != TerminatorKind::Branch || !isVisited.add(terminator.conditionOperand().index()))
            continue;
        const Value& condition = rewrite.at(terminator.conditionOperand());
        if (condition.opcode != Opcode::Boolean && condition.type == TypeTable::boolean) {
            isUndecided.quickSet(terminator.conditionOperand().index());
            undecidedConditions.append(terminator.conditionOperand());
        }
    }
    if (undecidedConditions.isEmpty() || (!rewrite.analyses.computed().contains(FunctionAnalysis::UseLists) && !isAnyUsedTwice(rewrite, isUndecided)))
        return outcomeOfMerging;
    for (ValueID condition : undecidedConditions)
        propagateCondition(rewrite, condition);
    return rewrite.commit() == PassOutcome::Changed ? PassOutcome::Changed : outcomeOfMerging;
}

// SinkConstants.

namespace {

struct ConstantSinker {
    Rewrite& rewrite;
    std::optional<StronglyConnectedComponents> components { };
    Vector<BlockID, 4> userBlocks { };
    Vector<ValueID> blockStamps { };

    bool isCandidate(ValueID id)
    {
        return resultRuleOf(rewrite.at(id).opcode) == ResultRule::Constant && !rewrite.analyses.effectsOf(id).onAnyEdge().interferesWith(Effects::unknownCode());
    }

    bool isOnlyHandedOnOverSomeEdges(ValueID id, BlockID block)
    {
        const UseLists& uses = rewrite.uses();
        if (uses.locationOf(id).placement != ValuePlacement::InBlock || uses.blockOf(id) != block || !isCandidate(id) || rewrite.analyses.hasNoEffects(id))
            return false;
        BitVector targets;
        for (const Use& use : uses.usesOf(id)) {
            if (use.kind != Use::Kind::EdgeArgument || use.block != block)
                return false;
            targets.set(use.targetIndex);
        }
        return targets.bitCount() < rewrite.at(block).terminator.targets.size();
    }

    // A constant that nothing but the terminator of its own block uses, and that goes over some of the edges only, is created on those edges.
    void splitEdgesThatHandOnConstants()
    {
        Vector<Edge, 4> edges;
        for (BlockID block : rewrite.blocks()) {
            const Terminator& terminator = rewrite.at(block).terminator;
            for (uint32_t targetIndex = 0; terminator.targets.size() >= 2 && targetIndex < terminator.targets.size(); ++targetIndex) {
                bool handsOnConstant = std::ranges::any_of(terminator.targets[targetIndex].arguments, [&](ValueID argument) {
                    return resultRuleOf(rewrite.at(argument).opcode) == ResultRule::Constant && isOnlyHandedOnOverSomeEdges(argument, block);
                });
                if (handsOnConstant)
                    edges.append(Edge { block, targetIndex });
            }
        }
        for (Edge edge : edges)
            rewrite.builder().splitEdge(edge);
    }

    // What a block hands to its handler, or is entered with, has to be there at its head. An immediate that a terminator names costs nothing wherever it is defined.
    bool canTakeCopy(const Use& use, ValueID id)
    {
        return use.kind == Use::Kind::Operand || ((use.kind == Use::Kind::TerminatorOperand || use.kind == Use::Kind::EdgeArgument) && !rewrite.analyses.hasNoEffects(id));
    }

    bool canSink(ValueID id, BlockID definitionBlock, std::span<const Use> uses)
    {
        userBlocks.shrink(0);
        for (const Use& use : uses) {
            if (!canTakeCopy(use, id) || use.block == definitionBlock)
                return false;
            if (rewrite.analyses.isInCycle(use.block) && !rewrite.analyses.blockCycles().isInEveryCycleOf(definitionBlock, use.block))
                return false;
            if (userBlocks.isEmpty() || userBlocks.last() != use.block)
                userBlocks.append(use.block);
        }
        return userBlocks.size() == 1 || (!isRematerializable(rewrite.at(id)) && hasExclusiveUserBlocks(id));
    }

    bool hasExclusiveUserBlocks(ValueID candidate)
    {
        const CFG& cfg = rewrite.cfg();
        if (!components)
            components = computeStronglyConnectedComponents(cfg.numNodes(), [&](unsigned index) { return cfg.successors(BlockID(index)); });
        const Vector<uint32_t>& componentOf = components->componentOf;
        if (blockStamps.isEmpty())
            blockStamps.fill(ValueID(), rewrite.function.blocks.size());
        uint32_t minComponent = std::numeric_limits<uint32_t>::max();
        Vector<BlockID, 16> worklist;
        for (BlockID block : userBlocks) {
            minComponent = std::min(minComponent, componentOf[block.index()]);
            worklist.append(block);
        }
        while (!worklist.isEmpty()) {
            for (BlockID target : cfg.successors(worklist.takeLast())) {
                if (componentOf[target.index()] < minComponent || blockStamps[target.index()] == candidate)
                    continue;
                blockStamps[target.index()] = candidate;
                worklist.append(target);
            }
        }
        return !userBlocks.containsIf([&](BlockID block) { return blockStamps[block.index()] == candidate; });
    }

    // Where the users lie in a cycle or reach each other, one value serves them all: it is created in the lowest block outside of every cycle that dominates them.
    std::optional<ProgramPoint> findPointThatServesAll(ValueID original, BlockID definitionBlock, std::span<const Use> uses)
    {
        if (isRematerializable(rewrite.at(original)) || rewrite.analyses.isInCycle(definitionBlock))
            return std::nullopt;
        const Dominators& dominators = rewrite.analyses.dominators();
        BlockID target;
        for (const Use& use : uses) {
            if (!canTakeCopy(use, original) || use.block == definitionBlock)
                return std::nullopt;
            target = target ? dominators.nearestCommonDominator(target, use.block) : use.block;
        }
        // What a block with a handler creates is not there yet when the handler is entered.
        auto servesEveryUser = [&](BlockID block) {
            return !rewrite.analyses.isInCycle(block) && std::ranges::all_of(uses, [&](const Use& use) { return use.block == block || dominators.terminatorDominates(block, use.block); });
        };
        while (target != definitionBlock && !servesEveryUser(target))
            target = dominators.idom(target);
        if (target == definitionBlock)
            return std::nullopt;
        auto firstUseInTarget = std::ranges::find_if(uses, [&](const Use& use) { return use.block == target; });
        return firstUseInTarget != uses.end() ? rewrite.uses().pointOf(*firstUseInTarget) : pointBeforeTerminatorOf(rewrite.function, target);
    }

    void copyLater(BlockID block, ValueID original, ProgramPoint point, std::span<const Use> uses)
    {
        rewrite.later([&rewrite = rewrite, block, original, point, uses = rewrite.keep(uses)] {
            ValueID copy = rewrite.insertBefore(point, rewrite.at(original).origin(), Value(rewrite.at(original)));
            for (const Use& use : uses)
                rewrite.replaceUse(use, copy);
            rewrite.erase(block, original);
        });
    }
};

} // anonymous namespace

PassOutcome sinkConstants(FunctionPassContext& context)
{
    if (context.function.blocks.size() < 2)
        return PassOutcome::Unchanged;
    Rewrite rewrite(context);
    ConstantSinker sinker { rewrite };
    sinker.splitEdgesThatHandOnConstants();
    const UseLists& useLists = rewrite.uses();
    for (BlockID block : rewrite.blocks()) {
        for (ValueID id : rewrite.at(block).values) {
            std::span<const Use> uses = sinker.isCandidate(id) ? useLists.usesOf(id) : std::span<const Use> { };
            if (uses.empty())
                continue;
            if (!sinker.canSink(id, block, uses)) {
                if (std::optional<ProgramPoint> point = sinker.findPointThatServesAll(id, block, uses))
                    sinker.copyLater(block, id, *point, uses);
                continue;
            }
            while (!uses.empty()) {
                size_t numberOfUses = std::ranges::find_if(uses, [&](const Use& use) { return use.block != uses.front().block; }) - uses.begin();
                sinker.copyLater(block, id, useLists.pointOf(uses.front()), uses.first(numberOfUses));
                uses = uses.subspan(numberOfUses);
            }
        }
    }
    return rewrite.commit();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
