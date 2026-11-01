/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCodeSizeEstimate.h"
#include "AOTDecidedBranches.h"
#include "AOTHeat.h"
#include "AOTKnownValue.h"
#include "AOTNaturalLoops.h"
#include "AOTRegionCloner.h"
#include "AOTRewrite.h"
#include "AOTSSARepair.h"
#include "AOTTypeTestPlan.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <wtf/HashMap.h>
#include <wtf/IndexMap.h>
#include <wtf/IndexSet.h>

namespace JSC { namespace AOT { namespace Typed {

// A copy saves a test each time the join is passed, and gives the passes behind it the function to clean up once more.
static constexpr HeatClass leastHeatOfJoinsCopiedBesideSlowPaths = HeatClass::Hot;
// The copy of a join above a test takes the test and the conversions of the value out of what runs, and adds only to what seldom runs.
static constexpr HeatClass leastHeatOfJoinsCopiedAboveTests = HeatClass::Warm;
// The join, the blocks that read what stands between it and the test, and the block with the test.
static constexpr size_t maxBlocksFromJoinToTest = 4;

static bool hasRedirectableEdges(TerminatorKind kind)
{
    switch (kind) {
    case TerminatorKind::Jump:
    case TerminatorKind::Branch:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::SwitchOnShape:
        return true;
    case TerminatorKind::ReturnCall:
    case TerminatorKind::Return:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
        return false;
    case TerminatorKind::TestAndBranch:
    case TerminatorKind::TryCallInline:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ValueID valueTestedBy(const Terminator& terminator)
{
    switch (terminator.kind) {
    case TerminatorKind::Branch:
        return terminator.conditionOperand();
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
        return terminator.subjectOperand();
    case TerminatorKind::Jump:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::ReturnCall:
    case TerminatorKind::Return:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
        return { };
    case TerminatorKind::TestAndBranch:
    case TerminatorKind::TryCallInline:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasEdgeTo(const Terminator& terminator, BlockID block)
{
    return std::ranges::contains(terminator.targets, block, &Target::block);
}

static bool isLoopHeader(const CFG& cfg, BlockID block)
{
    uint32_t orderIndex = cfg.reversePostOrderIndex(block);
    return std::ranges::any_of(cfg.incomingEdges(block), [&](Edge edge) {
        return cfg.reversePostOrderIndex(edge.from) >= orderIndex;
    });
}

static ValueID substituteParameter(const Block& mergePoint, ValueID value, const Arguments& incomingArguments)
{
    size_t index = mergePoint.parameters.find(value);
    return index == notFound ? value : incomingArguments[index];
}

static Target substituteParameters(const Block& mergePoint, BlockID target, const Arguments& passed, const Arguments& incomingArguments)
{
    Target result;
    result.block = target;
    result.arguments.reserveInitialCapacity(passed.size());
    for (ValueID argument : passed)
        result.arguments.append(substituteParameter(mergePoint, argument, incomingArguments));
    return result;
}

// Each run of copyRepeatedTests() takes one step along a row of equal tests, and every step costs a round of the passes beside this one. A row of calls of one function is short.
static const Value* findFunctionTestBranchedOn(const FunctionPassContext& context, const Block& block)
{
    const Value* test = typeTestBranchedOn(context.function, block);
    return test && holdsOnlyKnownFunctions(context.types.at(test->testedType())) ? test : nullptr;
}

static bool hasTwoFunctionTestsForOneType(const FunctionPassContext& context)
{
    Vector<TypeID, 8> testedTypes;
    for (const Block& block : context.function.blocks) {
        const Value* test = findFunctionTestBranchedOn(context, block);
        if (!test)
            continue;
        if (testedTypes.contains(test->testedType()))
            return true;
        testedTypes.append(test->testedType());
    }
    return false;
}

namespace {

// An edge into a merge point, and the target that the test there chooses for what the edge passes.
using DecidedEdge = std::pair<Edge, uint32_t>;

// One phase: the merge points that it has chosen and the edges into them that it may point elsewhere. An edge moves as soon as it is decided and the analyses are told at once, so the frame only lends its reads and its builder.
struct EdgeThreader : Rewrite {
    using Rewrite::Rewrite;

    bool canRedirect(Edge edge) const
    {
        if (edge.isHandlerEdge() || hasEdgeResults(function, edge))
            return false;
        return edge.from != targetOf(function, edge).block && hasRedirectableEdges(at(edge.from).terminator.kind);
    }

    bool chooseMergePoints(NOESCAPE const Invocable<bool(const CFG&, BlockID)> auto& isWanted)
    {
        return chooseMergePointsAmong(analyses.cfg().reversePostOrder(), isWanted);
    }

    bool chooseMergePointsAmong(std::span<const BlockID> blocks, NOESCAPE const Invocable<bool(const CFG&, BlockID)> auto& isWanted)
    {
        const CFG& cfg = analyses.cfg();
        for (BlockID block : blocks) {
            if (cfg.incomingEdges(block).size() >= 2 && isWanted(cfg, block))
                candidates.append(block);
        }
        if (candidates.isEmpty())
            return false;
        incomingEdges = IndexMap<BlockID, Vector<Edge, 2>>(function.blocks.size());
        for (BlockID candidate : candidates) {
            isCandidate.add(candidate);
            for (Edge edge : cfg.incomingEdges(candidate)) {
                if (canRedirect(edge))
                    incomingEdges[candidate].append(edge);
            }
        }
        return true;
    }

    void didAddEdge(Edge edge)
    {
        BlockID target = targetOf(function, edge).block;
        if (isCandidate.contains(target) && canRedirect(edge))
            incomingEdges[target].append(edge);
    }

    std::optional<Target> createThreadedTarget(const Block& mergePoint, Edge chosenEdge, const Arguments& incomingArguments) const
    {
        Target target = substituteParameters(mergePoint, targetOf(function, chosenEdge).block, argumentsWithoutTest(function, chosenEdge), incomingArguments);
        if (hasEdgeResults(function, chosenEdge) && !argumentsFitParameters(program, function, target))
            return std::nullopt;
        return target;
    }

    void redirect(Edge edge, Target&& target)
    {
        raiseFrequencyBehind(edge, target.block);
        builder().redirectEdge(edge, WTF::move(target));
        didAddEdge(edge);
        outcome = PassOutcome::Changed;
    }

    // A block is rare because the test above it seldom sends anybody there. Whoever passes the test by, because its outcome is known on his way, always goes there.
    // The frequency of a block also stands for the edges into it, so nothing changes for a way that is itself unlikely, nor for a block that others than that test lead to.
    // How often the way itself is taken nobody knows. In a loop that matters: what is held for unlikely there is sent round another header, and the loop is entered anew behind it.
    void raiseFrequencyBehind(Edge edge, BlockID block)
    {
        BlockID passedBy = targetOf(function, edge).block;
        Expected<BlockFrequency> frequency = rarerOf(at(edge.from).frequency, at(passedBy).frequency);
        if (frequency.predicted() != BlockFrequency::Normal)
            return;
        for (BlockID from = passedBy; isRarerThan(at(block).frequency, frequency).predicted(); block = at(block).terminator.targets[0].block) {
            for (Edge incoming : analyses.cfg().incomingEdges(block)) {
                if (incoming.from != from)
                    return;
            }
            const NaturalLoop* loop = analyses.naturalLoops().innerMostLoopOf(block);
            if (!loop || loop != analyses.naturalLoops().innerMostLoopOf(edge.from))
                return;
            builder().function().edit(block, BlockEdit::Rest).frequency = frequency;
            if (at(block).terminator.kind != TerminatorKind::Jump)
                return;
            from = block;
        }
    }

    // Each of the copies is one more definition of what the original beside it stands for. The variables of one block follow each other, and the first copy of the block makes them.
    void addDefinitions(BlockID block, const Arguments& originals, const Arguments& copies)
    {
        if (!repair)
            repair.emplace(builder(), analyses);
        size_t first = firstVariableOfBlock.ensure(block.index(), [&] { return variables.size(); }).iterator->value;
        for (unsigned index = 0; index < originals.size(); ++index) {
            if (first + index == variables.size())
                variables.append(repair->addVariable(originals[index]));
            repair->addDefinition(variables[first + index], copies[index]);
        }
    }

    // The copy of the first block.
    BlockID copyBlocks(std::span<const BlockID> originals)
    {
        RegionCloner cloner(builder(), function);
        cloner.cloneBlocks(originals, [](ValueID, Value&) { });

        for (BlockID original : originals) {
            Arguments definitions = at(original).parameters;
            for (ValueID value : at(original).values) {
                if (resultRuleOf(at(value).opcode) != ResultRule::None)
                    definitions.append(value);
            }
            Arguments copies;
            for (ValueID definition : definitions)
                copies.append(cloner.cloneOf(definition));
            addDefinitions(original, definitions, copies);
        }
        return cloner.cloneOf(originals[0]);
    }

    BlockID copyBlock(BlockID original) { return copyBlocks(singleElementSpan(original)); }

    PassOutcome finish()
    {
        if (repair) {
            builder().emptyUnreachableBlocks();
            repair->run();
        }
        return outcome;
    }

    Vector<BlockID, 8> copyTargetsOf(std::span<const Edge> decidedEdges)
    {
        Vector<BlockID, 8> copies;
        for (Edge edge : decidedEdges) {
            copies.append(copyBlock(targetOf(function, edge).block));
            redirect(edge, Target { copies.last(), Arguments(targetOf(function, edge).arguments), { } });
        }
        return copies;
    }

    // An edge that decides the test of a merge point gets a copy of the block for itself. There the test folds.
    PassOutcome giveCopiesTo(std::span<const Edge> decidedEdges)
    {
        copyTargetsOf(decidedEdges);
        return finish();
    }

    // A copy that still goes both ways brings together what the test before it has parted, and each value alive there gets a parameter: in a row of equal tests, the square of the row.
    // So the copies are decided as the next round would decide them, before the values are repaired.
    void decideTypeTestsOf(std::span<const BlockID> copies)
    {
        TypeTestBranches typeTests(analyses);
        Vector<Edge, 8> decided;
        for (BlockID copy : copies) {
            KnownBoolean outcome = typeTests.outcomeFromDominatingTest(copy);
            if (outcome == KnownBoolean::Unknown)
                outcome = typeTests.outcomeOnEveryWay(copy);
            if (outcome == KnownBoolean::Unknown)
                continue;
            Edge chosen { copy, outcome == KnownBoolean::True ? 0u : 1u };
            if (canDecideTerminator(program, function, builder().types(), chosen))
                decided.append(chosen);
        }
        for (Edge chosen : decided)
            raiseFrequenciesBehind(analyses, builder().function(), chosen);
        for (Edge chosen : decided)
            decideTerminator(builder(), chosen.from, chosen.targetIndex);
        for (Edge chosen : decided)
            castArgumentsToParameters(builder(), chosen.from, context.build.validates());
        builder().applyInsertions();
    }

    PassOutcome threadBranchesOnParameters()
    {
        IndexSet<BlockID> copiedCandidates;
        bool hasCandidates = chooseMergePoints([&](const CFG&, BlockID id) {
            const Block& block = at(id);
            ValueID tested = valueTestedBy(block.terminator);
            if (!tested || block.isHandlerBlock() || !block.parameters.contains(tested))
                return false;
            const UseLists& useLists = analyses.useLists();
            if (useLists.useCount(tested) != 1)
                return false;
            bool parametersDieInBlock = std::ranges::all_of(block.parameters, [&](ValueID parameter) {
                return std::ranges::all_of(useLists.usesOf(parameter), [&](const Use& use) {
                    return use.block == id;
                });
            });
            if (parametersDieInBlock && block.values.isEmpty())
                return true;

            // The dispatch of a state machine carries every live value. Skipping it is worth a copy of the block for each state, as long as a copy has no code.
            if (block.terminator.kind != TerminatorKind::Switch)
                return false;
            bool hasCode = std::ranges::any_of(block.values, [&](ValueID value) {
                return estimatedCodeBytes(program, function, at(value));
            });
            if (hasCode || RegionCloner::checkCanDuplicate(analyses, singleElementSpan(id)) != CloneCheck::CanClone)
                return false;
            copiedCandidates.add(id);
            return true;
        });
        if (!hasCandidates)
            return PassOutcome::Unchanged;

        const CFG& cfg = analyses.cfg();
        auto findDecidedEdges = [&](BlockID candidate) {
            const Block& mergePoint = at(candidate);
            size_t testedIndex = mergePoint.parameters.find(valueTestedBy(mergePoint.terminator));
            Vector<DecidedEdge, 2> decidedEdges;
            for (Edge edge : incomingEdges[candidate]) {
                KnownValue known = knownValueOf(program, function, targetOf(function, edge).arguments[testedIndex]);
                std::optional<uint32_t> targetIndex = decidedTargetOf(mergePoint.terminator, known);
                if (targetIndex && !hasEdgeResults(function, Edge { candidate, *targetIndex }))
                    decidedEdges.append({ edge, *targetIndex });
            }
            return decidedEdges;
        };
        // An edge that skips the header of a loop enters the loop at a second block, unless the header goes away and every edge from outside takes the same way in.
        auto leavesLoopWithOneEntry = [&](BlockID header) {
            Vector<DecidedEdge, 2> decidedEdges = findDecidedEdges(header);
            if (decidedEdges.size() != cfg.incomingEdges(header).size())
                return false;

            std::optional<uint32_t> entry;
            for (auto [edge, targetIndex] : decidedEdges) {
                if (cfg.reversePostOrderIndex(edge.from) >= cfg.reversePostOrderIndex(header))
                    continue;
                if (entry.value_or(targetIndex) != targetIndex)
                    return false;
                entry = targetIndex;
            }
            return true;
        };
        // Decided before the first edge moves: a block that every edge has skipped is unreachable and still lists its own edges.
        IndexSet<BlockID> keptLoopHeaders;
        for (BlockID candidate : candidates) {
            if (copiedCandidates.contains(candidate) && isLoopHeader(cfg, candidate) && !leavesLoopWithOneEntry(candidate))
                keptLoopHeaders.add(candidate);
        }

        Vector<std::pair<Edge, BlockID>, 4> copies;
        auto ensureCopy = [&](BlockID original, uint32_t targetIndex) {
            for (auto [decided, existing] : copies) {
                if (decided.from == original && decided.targetIndex == targetIndex)
                    return existing;
            }

            BlockID copy = copyBlock(original);
            copies.append({ Edge { original, targetIndex }, copy });
            Terminator dispatch = std::exchange(function.edit(copy, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator, Terminator { });
            Target& chosen = dispatch.targets[targetIndex];
            builder().setOrigin(dispatch.origin());
            builder().moveToEndOf(copy);
            builder().emitJump(chosen.block, WTF::move(chosen.arguments));
            // The builder reports the jump, not the terminator in whose place it stands.
            analyses.didChangeControlFlow();
            didAddEdge(Edge { copy, 0 });
            return copy;
        };
        auto replaceJumpWithBranch = [&](BlockID source, const Block& mergePoint) {
            const Terminator& branch = mergePoint.terminator;
            Terminator jump = std::exchange(function.edit(source, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator, Terminator { });
            const Arguments& incomingArguments = jump.next().arguments;
            ValueID condition = substituteParameter(mergePoint, branch.conditionOperand(), incomingArguments);
            Target taken = substituteParameters(mergePoint, branch.taken().block, branch.taken().arguments, incomingArguments);
            Target notTaken = substituteParameters(mergePoint, branch.notTaken().block, branch.notTaken().arguments, incomingArguments);

            builder().setOrigin(jump.origin());
            builder().moveToEndOf(source);
            builder().emitBranch(condition, WTF::move(taken), WTF::move(notTaken));
            // The builder reports the branch, not the terminator in whose place it stands.
            analyses.didChangeControlFlow();
            didAddEdge(Edge { source, 0 });
            didAddEdge(Edge { source, 1 });
            outcome = PassOutcome::Changed;
        };

        for (BlockID candidate : candidates) {
            if (hasEdgeTo(at(candidate).terminator, candidate) || keptLoopHeaders.contains(candidate))
                continue;
            if (copiedCandidates.contains(candidate)) {
                for (auto [edge, targetIndex] : findDecidedEdges(candidate)) {
                    Arguments arguments = targetOf(function, edge).arguments;
                    BlockID copy = ensureCopy(candidate, targetIndex);
                    redirect(edge, Target { copy, WTF::move(arguments), { } });
                }
                continue;
            }
            const Block& mergePoint = at(candidate);
            const Terminator& terminator = mergePoint.terminator;
            ValueID tested = valueTestedBy(terminator);
            for (Edge edge : incomingEdges[candidate]) {
                const Arguments& incomingArguments = targetOf(function, edge).arguments;
                KnownValue known = knownValueOf(program, function, substituteParameter(mergePoint, tested, incomingArguments));
                if (std::optional<uint32_t> targetIndex = decidedTargetOf(terminator, known)) {
                    Edge chosenEdge { candidate, *targetIndex };
                    std::optional<Target> target = canFollowWithoutTest(function, chosenEdge) ? createThreadedTarget(mergePoint, chosenEdge, incomingArguments) : std::nullopt;
                    if (target)
                        redirect(edge, WTF::move(*target));
                } else if (terminator.kind == TerminatorKind::Branch && at(edge.from).terminator.kind == TerminatorKind::Jump)
                    replaceJumpWithBranch(edge.from, mergePoint);
            }
        }
        return finish();
    }

    bool canSkip(const Block& block)
    {
        ValueID tested = valueTestedBy(block.terminator);
        if (!tested || block.isHandlerBlock() || block.parameters.isEmpty())
            return false;
        if (!block.parameters.contains(tested) && !block.values.contains(tested))
            return false;
        bool canRemoveValues = std::ranges::all_of(block.values, [&](ValueID value) {
            return analyses.isRemovableWhenUnused(value);
        });
        return (canRemoveValues || context.build.isSabotaged(Sabotage::SkippedBlocksHaveEffects)) && !context.exceeds(LimitID::MaxThreadedBlockValues, block.values.size(), tested);
    }

    KnownValue evaluateBranch(const Block& mergePoint, const Arguments& incomingArguments) const
    {
        Vector<std::pair<ValueID, KnownValue>, 8> known;
        auto lookUp = [&](ValueID id) -> KnownValue {
            for (auto& [value, knownValue] : known) {
                if (value == id)
                    return knownValue;
            }
            return knownValueOf(program, function, substituteParameter(mergePoint, id, incomingArguments));
        };
        for (ValueID id : mergePoint.values) {
            KnownValue value = evaluateWithKnownOperands(program, function, at(id), lookUp);
            known.append({ id, value.isKnown() ? value : evaluateTestOfArgument(mergePoint, at(id), incomingArguments) });
        }
        return lookUp(valueTestedBy(mergePoint.terminator));
    }

    // The type of what an edge passes may decide a test that the type of the parameter leaves open.
    KnownValue evaluateTestOfArgument(const Block& mergePoint, const Value& test, const Arguments& incomingArguments) const
    {
        if ((test.opcode != Opcode::TypeTest && test.opcode != Opcode::IsClosureOf) || !mergePoint.parameters.contains(test.valueOperand()))
            return { };
        const Type& passed = typeOf(substituteParameter(mergePoint, test.valueOperand(), incomingArguments));
        KnownBoolean result = test.opcode == Opcode::TypeTest
            ? evaluateTypeTest(program, passed, context.types.at(test.testedType()))
            : evaluateIsClosureOf(program, passed, test.function());
        switch (result) {
        case KnownBoolean::Unknown:
            return { };
        case KnownBoolean::False:
            return KnownValue::boolean(false);
        case KnownBoolean::True:
            return KnownValue::boolean(true);
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    PassOutcome threadDecidedBranches()
    {
        bool hasCandidates = chooseMergePoints([&](const CFG& cfg, BlockID block) {
            const Block& mergePoint = at(block);
            return !isLoopHeader(cfg, block) && canSkip(mergePoint) && std::ranges::any_of(cfg.incomingEdges(block), [&](Edge edge) {
                return canRedirect(edge) && decidedTargetOf(mergePoint.terminator, evaluateBranch(mergePoint, targetOf(function, edge).arguments)).has_value();
            });
        });
        if (!hasCandidates)
            return PassOutcome::Unchanged;
        bool hasHandlers = std::ranges::any_of(analyses.cfg().reversePostOrder(), [&](BlockID block) {
            return at(block).isHandlerBlock();
        });

        // A constant that stands beside the test is no reason to pass through the block: it is made again where it is used.
        // So is what a decided test left there, a cast of a value from above the block behind a fact. It has no code.
        auto isCastLeftByTest = [&](const Block& block, const Value& cast) {
            return cast.opcode == Opcode::AssumeType && cast.factOperand() && at(cast.factOperand()).opcode == Opcode::Specified
                && !block.parameters.contains(cast.valueOperand()) && !block.values.contains(cast.valueOperand());
        };
        const UseLists& useListsBeforeCopies = analyses.useLists();
        Vector<std::tuple<Use, ValueID, ProgramPoint>, 8> usesOutsideBlocks;
        for (BlockID candidate : candidates) {
            for (ValueID id : at(candidate).values) {
                if (!isRematerializable(at(id)) && !isCastLeftByTest(at(candidate), at(id)))
                    continue;
                for (const Use& use : useListsBeforeCopies.usesOf(id)) {
                    bool hasPointInBlock = use.kind == Use::Kind::Operand || use.kind == Use::Kind::TerminatorOperand || use.kind == Use::Kind::EdgeArgument;
                    if (use.block != candidate && hasPointInBlock)
                        usesOutsideBlocks.append({ use, id, useListsBeforeCopies.pointOf(use) });
                }
            }
        }
        // The first insertion ends the life of the use lists.
        for (const auto& [use, original, point] : usesOutsideBlocks) {
            Value copy(at(original));
            if (copy.opcode == Opcode::AssumeType)
                copy.setFactOperand(insertBefore(point, copy.origin(), Value(Opcode::Specified)));
            replaceUse(use, insertBefore(point, copy.origin(), WTF::move(copy)));
        }
        if (!usesOutsideBlocks.isEmpty()) {
            builder().applyInsertions();
            // The builder reports the copies, not the uses that now name them.
            analyses.didChangeValues();
            outcome = PassOutcome::Changed;
        }

        const UseLists& useLists = analyses.useLists();
        IndexMap<ValueID, BlockID> definingCandidate(function.values.size());
        IndexMap<BlockID, IndexSet<BlockID>> usersOutsideBlock(function.blocks.size());
        IndexSet<BlockID> computesValuesForOthers;
        IndexMap<BlockID, uint32_t> orderOfBlock(function.blocks.size());
        for (uint32_t order = 0; BlockID block : analyses.cfg().reversePostOrder())
            orderOfBlock[block] = order++;
        IndexMap<ValueID, uint32_t> orderBehindLastUser(function.values.size());
        auto addUserOutsideBlock = [&](BlockID candidate, ValueID definition, BlockID user) {
            if (user == candidate)
                return;
            usersOutsideBlock[candidate].add(user);
            orderBehindLastUser[definition] = std::max(orderBehindLastUser[definition], orderOfBlock[user] + 1);
            if (!at(candidate).parameters.contains(definition))
                computesValuesForOthers.add(candidate);
        };
        for (BlockID candidate : candidates) {
            auto addUsersOf = [&](ValueID definition) {
                definingCandidate[definition] = candidate;
                for (const Use& use : useLists.usesOf(definition))
                    addUserOutsideBlock(candidate, definition, use.block);
            };
            for (ValueID parameter : at(candidate).parameters)
                addUsersOf(parameter);
            for (ValueID value : at(candidate).values)
                addUsersOf(value);
        }

        IndexMap<BlockID, std::pair<uint32_t, uint32_t>> numbersInSubtree(function.blocks.size());
        uint32_t number = 0;
        analyses.dominators().walkTree([&](BlockID block) {
            numbersInSubtree[block].first = ++number;
        }, [&](BlockID block) {
            numbersInSubtree[block].second = number;
        });
        IndexMap<BlockID, BlockID> dominatorOfCandidate(function.blocks.size());
        for (BlockID candidate : candidates)
            dominatorOfCandidate[candidate] = analyses.dominators().idom(candidate);
        // A block made since the tree was numbered counts as dominated.
        auto wasDominatedBy = [&](BlockID candidate, BlockID block) {
            if (block.index() >= numbersInSubtree.size())
                return true;
            auto [first, last] = numbersInSubtree[candidate];
            return numbersInSubtree[block].first >= first && numbersInSubtree[block].first <= last;
        };

        enum class UsersBelow : uint8_t { None, Some, Unknown };
        // The block dominates its users, so a way to a user that does not pass through the block leads only through what the block dominates.
        auto findUsersBelow = [&](Edge chosenEdge) {
            BlockID candidate = chosenEdge.from;
            if (!canFollowWithoutTest(function, chosenEdge))
                return UsersBelow::Unknown;
            bool passesValuesOfBlock = argumentsWithoutTest(function, chosenEdge).containsIf([&](ValueID argument) {
                return at(candidate).values.contains(argument);
            });
            if (passesValuesOfBlock)
                return UsersBelow::Unknown;

            BlockID start = targetOf(function, chosenEdge).block;
            const IndexSet<BlockID>& users = usersOutsideBlock[candidate];
            if (users.isEmpty() || !wasDominatedBy(candidate, start))
                return UsersBelow::None;
            IndexSet<BlockID> seen;
            seen.add(candidate);
            seen.add(start);
            Vector<BlockID, 16> worklist { start };
            uint64_t remainingSteps = context.limit(LimitID::MaxThreadingReachabilitySteps);
            while (!worklist.isEmpty()) {
                BlockID block = worklist.takeLast();
                if (users.contains(block))
                    return UsersBelow::Some;
                if (!remainingSteps--) {
                    context.gaveUp(LimitID::MaxThreadingReachabilitySteps, valueTestedBy(at(candidate).terminator));
                    return UsersBelow::Unknown;
                }
                at(block).forEachTarget([&](const Target& target) {
                    if (wasDominatedBy(candidate, target.block) && seen.add(target.block))
                        worklist.append(target.block);
                });
            }
            return UsersBelow::None;
        };

        Vector<DecidedEdge, 4> skippingEdges;
        IndexSet<BlockID> skippedBlocks;
        IndexSet<BlockID> blocksToSkip;
        for (BlockID candidate : candidates) {
            const Block& mergePoint = at(candidate);
            const Terminator& terminator = mergePoint.terminator;
            if (hasEdgeTo(terminator, candidate))
                continue;
            Vector<std::optional<UsersBelow>, 2> usersBelowTargets(FillWith { }, terminator.targets.size(), std::nullopt);
            for (Edge edge : incomingEdges[candidate]) {
                const Arguments& incomingArguments = targetOf(function, edge).arguments;
                std::optional<uint32_t> targetIndex = decidedTargetOf(terminator, evaluateBranch(mergePoint, incomingArguments));
                if (!targetIndex)
                    continue;
                Edge chosenEdge { candidate, *targetIndex };
                std::optional<UsersBelow>& usersBelow = usersBelowTargets[*targetIndex];
                if (!usersBelow)
                    usersBelow = findUsersBelow(chosenEdge);
                if (*usersBelow == UsersBelow::Some) {
                    skippingEdges.append({ edge, *targetIndex });
                    blocksToSkip.add(candidate);
                }
                std::optional<Target> target = *usersBelow == UsersBelow::None ? createThreadedTarget(mergePoint, chosenEdge, incomingArguments) : std::nullopt;
                if (!target)
                    continue;
                for (ValueID argument : target->arguments) {
                    if (BlockID definingBlock = definingCandidate[argument])
                        addUserOutsideBlock(definingBlock, argument, edge.from);
                }
                redirect(edge, WTF::move(*target));
                skippedBlocks.add(candidate);
            }
        }

        // A block that edges skip no longer dominates what lies below it: every join there merges its parameters again, down to the next block that no edge skips. Such a block ends a row of skipped ones.
        IndexSet<BlockID> blocksThatEndRows;
        if (!skippingEdges.isEmpty() && !hasHandlers) {
            IndexMap<BlockID, Vector<uint32_t>> valuesMergedAgainBelow(function.blocks.size());
            for (BlockID candidate : candidates) {
                Vector<uint32_t> mergedAgain;
                for (uint32_t orderBehindLastUserOfValue : valuesMergedAgainBelow[dominatorOfCandidate[candidate]]) {
                    if (orderBehindLastUserOfValue > orderOfBlock[candidate])
                        mergedAgain.append(orderBehindLastUserOfValue);
                }
                if (blocksToSkip.contains(candidate) && !computesValuesForOthers.contains(candidate)) {
                    size_t numberFromAbove = mergedAgain.size();
                    for (ValueID parameter : at(candidate).parameters) {
                        if (orderBehindLastUser[parameter])
                            mergedAgain.append(orderBehindLastUser[parameter]);
                    }
                    if (context.exceeds(LimitID::MaxValuesMergedAgainBelowThreadedJumps, mergedAgain.size(), valueTestedBy(at(candidate).terminator))) {
                        blocksThatEndRows.add(candidate);
                        mergedAgain.shrink(numberFromAbove);
                    } else
                        skippedBlocks.add(candidate);
                }
                if (skippedBlocks.contains(candidate))
                    valuesMergedAgainBelow[candidate] = WTF::move(mergedAgain);
            }
        }

        // The edge goes through a block of its own with the parameters of the block that it skips: each is one more definition of what the parameter stands for, and the joins below get parameters for the two.
        for (auto [edge, targetIndex] : skippingEdges) {
            BlockID mergePoint = targetOf(function, edge).block;
            Edge chosenEdge { mergePoint, targetIndex };
            // An edge that skips the block has a value for each of its parameters. What the block computes exists only behind it.
            if (!isCandidate.contains(mergePoint) || hasHandlers || computesValuesForOthers.contains(mergePoint) || blocksThatEndRows.contains(mergePoint))
                continue;
            Arguments incomingArguments = targetOf(function, edge).arguments;
            if (!createThreadedTarget(at(mergePoint), chosenEdge, incomingArguments))
                continue;

            BlockID target = targetOf(function, chosenEdge).block;
            Arguments argumentsOfChosenEdge = argumentsWithoutTest(function, chosenEdge);
            builder().setOrigin(at(mergePoint).terminator.origin());
            BlockID skippingBlock = builder().addBlock(at(edge.from).frequency);
            Arguments parameters;
            // No edge leads to the block yet.
            for (ValueID argument : incomingArguments)
                parameters.append(builder().addParameter(skippingBlock, at(argument).type, 0));
            Target next = substituteParameters(at(mergePoint), target, argumentsOfChosenEdge, parameters);
            builder().moveToEndOf(skippingBlock);
            builder().emitJump(next.block, WTF::move(next.arguments));
            redirect(edge, Target { skippingBlock, WTF::move(incomingArguments), { } });
            addDefinitions(mergePoint, at(mergePoint).parameters, parameters);
        }
        return finish();
    }

    // The parameter whose type a test at the end of the block asks for. `p = p.next; p.m`: what refuses null and undefined may stand between the two.
    struct TestedParameter {
        ValueID parameter;
        bool isNeverNullish { false };
    };
    TestedParameter parameterTestedBy(const Block& block, const Value& test) const
    {
        ValueID tested = test.valueOperand();
        if (block.parameters.contains(tested))
            return { tested };
        if (context.isBehindRounds && at(tested).opcode == Opcode::RequireObjectCoercible && block.values.contains(tested) && block.parameters.contains(at(tested).valueOperand()))
            return { at(tested).valueOperand(), true };
        return { };
    }

    bool decidesTypeTest(const Block& block, const Value& test, Edge edge) const
    {
        TestedParameter tested = parameterTestedBy(block, test);
        const Type& passed = typeOf(substituteParameter(block, tested.parameter, targetOf(function, edge).arguments));
        return evaluateTypeTest(program, tested.isNeverNullish ? passed.withoutNullish() : passed, context.types.at(test.testedType())) != KnownBoolean::Unknown;
    }

    bool sparesLookIntoCell(const Block& block, const Value& test, Edge edge) const
    {
        TestedParameter tested = parameterTestedBy(block, test);
        auto looksIntoCells = [&](const Type& from) {
            return computeTypeTestPlanOfBoxedValue(program, tested.isNeverNullish ? from.withoutNullish() : from, context.types.at(test.testedType())).looksIntoCells();
        };
        return looksIntoCells(typeOf(tested.parameter)) && !looksIntoCells(typeOf(substituteParameter(block, tested.parameter, targetOf(function, edge).arguments)));
    }

    bool isSeldomTaken(Edge edge) const { return at(edge.from).frequency.predicted() == BlockFrequency::Rare; }

    // The one object that the likely ways make and pass, where an unlikely way passes something else: by itself the object need not be made.
    ValueID findObjectMadeButOnUnlikelyWays(const CFG& cfg, BlockID id, size_t position) const
    {
        ValueID passedOnLikelyWays;
        bool isPassedOnUnlikelyWay = false;
        for (Edge edge : cfg.incomingEdges(id)) {
            const Arguments& arguments = targetOf(function, edge).arguments;
            if (edge.isHandlerEdge() || position >= arguments.size())
                return { };
            if (at(edge.from).frequency.predicted() != BlockFrequency::Normal) {
                if (!canRedirect(edge))
                    return { };
                isPassedOnUnlikelyWay = true;
                continue;
            }
            if (passedOnLikelyWays && passedOnLikelyWays != arguments[position])
                return { };
            passedOnLikelyWays = arguments[position];
        }
        if (!isPassedOnUnlikelyWay || !passedOnLikelyWays)
            return { };
        const Value& made = at(passedOnLikelyWays);
        return made.opcode == Opcode::NewObject && !made.prototypeOperand() ? passedOnLikelyWays : ValueID();
    }

    // Where a test of the members of a class has failed the object is made by another road, and the object of the likely way would be joined with that one and made in every turn.
    // The next test of that class, which the join ends in, asks about no value, and its failing side relies on nothing: whoever takes the test for failed is right. So the unlikely way gets the block for itself and goes on there.
    PassOutcome sendUnlikelyWaysPastTestsOfMembers()
    {
        bool hasCandidates = chooseMergePoints([&](const CFG& cfg, BlockID id) {
            const Block& block = at(id);
            if (block.terminator.kind != TerminatorKind::Branch || block.parameters.isEmpty() || block.frequency.predicted() != BlockFrequency::Normal)
                return false;
            ValueID condition = block.terminator.conditionOperand();
            if (at(condition).opcode != Opcode::AreMembersUnchanged || !block.values.contains(condition))
                return false;
            if (isLoopHeader(cfg, id) || block.isHandlerBlock() || hasEdgeTo(block.terminator, id))
                return false;
            const Target& failed = block.terminator.notTaken();
            if (!failed.proofs.isEmpty() || hasEdgeResults(function, Edge { id, 1 }) || at(failed.block).frequency.predicted() == BlockFrequency::Normal)
                return false;
            bool leavesObjectAlone = false;
            for (size_t position = 0; position < block.parameters.size() && !leavesObjectAlone; ++position) {
                ValueID made = findObjectMadeButOnUnlikelyWays(cfg, id, position);
                leavesObjectAlone = made && std::ranges::any_of(at(condition).conditions(), [&](MemberConditionRecord asked) {
                    return asked.layout == declaredLayoutOf(program, at(made).layout());
                });
            }
            if (!leavesObjectAlone)
                return false;
            size_t numberOfCopies = std::ranges::count_if(cfg.incomingEdges(id), [&](Edge edge) { return at(edge.from).frequency.predicted() != BlockFrequency::Normal; });
            if (context.exceeds(LimitID::MaxThreadedBlockValues, block.values.size() * numberOfCopies, condition))
                return false;
            return RegionCloner::checkCanDuplicate(analyses, singleElementSpan(id)) == CloneCheck::CanClone;
        });
        if (!hasCandidates)
            return PassOutcome::Unchanged;

        Vector<Edge, 8> unlikelyEdges;
        for (BlockID candidate : candidates) {
            for (Edge edge : incomingEdges[candidate]) {
                if (at(edge.from).frequency.predicted() != BlockFrequency::Normal)
                    unlikelyEdges.append(edge);
            }
        }
        Vector<Expected<BlockFrequency>, 8> frequencies;
        for (Edge edge : unlikelyEdges)
            frequencies.append(at(edge.from).frequency);
        Vector<BlockID, 8> copies = copyTargetsOf(unlikelyEdges.span());
        for (size_t index = 0; index < copies.size(); ++index) {
            builder().function().edit(copies[index], BlockEdit::Rest).frequency = frequencies[index];
            decideTerminator(builder(), copies[index], 1);
        }
        for (BlockID copy : copies)
            castArgumentsToParameters(builder(), copy, context.build.validates());
        builder().applyInsertions();
        return finish();
    }

    // What an edge passes decides the test, but the block does more than test: it reads a variable that the code below uses, or may throw. No edge can skip that, and a copy of it costs less than the test and the merge.
    // A slow path that comes back knows nothing of what it passes. It keeps the block, and with it the test, which the other edges no longer pay for.
    // Only behind the rounds: PlaceTypeTests would copy the code below the join once more for the type that the copy knows.
    PassOutcome copySmallJoins()
    {
        bool hasCandidates = chooseMergePoints([&](const CFG& cfg, BlockID id) {
            const Block& block = at(id);
            if (blockBelow(block.terminator))
                joinsThatLeadToOneBlock.append(id);
            if (isLoopHeader(cfg, id) || block.isHandlerBlock() || hasEdgeTo(block.terminator, id) || block.frequency.predicted() != BlockFrequency::Normal)
                return false;
            ValueID condition = valueTestedBy(block.terminator);
            if (!condition)
                return false;
            // A finally block's statements come before the dispatch on how to continue, and every predecessor determines which way that goes.
            // What such a block creates for later code would be merged again there, and nothing is known about a closure or object that comes out of a merge.
            bool choosesByParameter = block.parameters.contains(condition)
                && !std::ranges::all_of(block.values, [&](ValueID value) { return analyses.isRemovableWhenUnused(value); })
                && std::ranges::all_of(block.values, [&](ValueID value) { return std::ranges::all_of(analyses.useLists().usesOf(value), [&](const Use& use) { return use.block == id; }); });
            const Value& test = at(condition);
            bool testsParameter = block.terminator.kind == TerminatorKind::Branch && test.opcode == Opcode::TypeTest && block.values.contains(condition) && parameterTestedBy(block, test).parameter;
            if (!choosesByParameter && !testsParameter)
                return false;
            size_t numberOfCopies = cfg.incomingEdges(id).size() - 1;
            if (context.exceeds(LimitID::MaxThreadedBlockValues, block.values.size() * numberOfCopies, condition))
                return false;
            if (RegionCloner::checkCanDuplicate(analyses, singleElementSpan(id)) != CloneCheck::CanClone)
                return false;
            bool holdsTestAlone = testsParameter && std::ranges::all_of(block.values, [&](ValueID value) {
                return value == condition || value == test.valueOperand() || !estimatedCodeBytes(program, function, at(value));
            });
            bool someEdgeGains = false;
            bool everyEdgeGainsOrIsSeldomTaken = std::ranges::all_of(cfg.incomingEdges(id), [&](Edge edge) {
                if (!canRedirect(edge))
                    return false;
                if (choosesByParameter)
                    return decidedTargetOf(block.terminator, knownValueOf(program, function, substituteParameter(block, condition, targetOf(function, edge).arguments))).has_value();
                if (!decidesTypeTest(block, test, edge)) {
                    if (!context.isBehindRounds || !holdsTestAlone || heatClassOf(context, id).predicted() < leastHeatOfJoinsCopiedBesideSlowPaths)
                        return false;
                    if (isSeldomTaken(edge))
                        return true;
                    if (!sparesLookIntoCell(block, test, edge))
                        return false;
                }
                someEdgeGains = true;
                return true;
            });
            return everyEdgeGainsOrIsSeldomTaken && (choosesByParameter || someEdgeGains);
        });
        if (!hasCandidates)
            return PassOutcome::Unchanged;

        Vector<Edge, 8> decidedEdges;
        Vector<std::pair<BlockID, Expected<BlockFrequency>>, 4> blocksKeptBySlowPaths;
        for (BlockID candidate : candidates) {
            const Block& block = at(candidate);
            const Value& test = at(valueTestedBy(block.terminator));
            std::span<const Edge> edges = incomingEdges[candidate].span();
            bool choosesByParameter = block.parameters.contains(valueTestedBy(block.terminator));
            size_t before = decidedEdges.size();
            Expected<BlockFrequency> frequencyOfRest = BlockFrequency::Rare;
            for (Edge edge : edges) {
                if (choosesByParameter || decidesTypeTest(block, test, edge) || !isSeldomTaken(edge))
                    decidedEdges.append(edge);
                else if (isRarerThan(frequencyOfRest, at(edge.from).frequency).predicted())
                    frequencyOfRest = at(edge.from).frequency;
            }
            if (decidedEdges.size() - before == edges.size())
                decidedEdges.removeLast();
            else
                blocksKeptBySlowPaths.append({ candidate, frequencyOfRest });
        }
        PassOutcome result = giveCopiesTo(decidedEdges.span());
        for (auto [block, frequency] : blocksKeptBySlowPaths)
            builder().function().edit(block, BlockEdit::Rest).frequency = frequency;
        return result;
    }

    static bool leadsToOneBlock(const Terminator& choice)
    {
        if (choice.kind != TerminatorKind::Branch && choice.kind != TerminatorKind::Switch)
            return false;
        return std::ranges::all_of(choice.targets, [&](const Target& target) { return target.block == choice.targets[0].block; });
    }

    bool isLikely(BlockID block) const { return at(block).frequency.predicted() == BlockFrequency::Normal; }

    // The one block that a choice leads to, but for the ways that are seldom taken.
    BlockID blockBelow(const Terminator& choice) const
    {
        if (leadsToOneBlock(choice))
            return choice.targets[0].block;
        if (!context.isBehindRounds)
            return { };
        if (choice.kind == TerminatorKind::Jump)
            return choice.targets[0].block;
        if (choice.kind != TerminatorKind::Branch)
            return { };
        BlockID below;
        for (const Target& target : choice.targets) {
            if (!isLikely(target.block))
                continue;
            if (below)
                return { };
            below = target.block;
        }
        return below;
    }

    struct TestBelowJoin {
        const Value* test { nullptr };
        ValueID parameterOfJoin;
        Vector<BlockID, maxBlocksFromJoinToTest> blocks;
    };

    // What the test asks about, as the join took it. The blocks in between may have handed it on as an argument.
    ValueID parameterOfJoinBehind(const TestBelowJoin& path, ValueID tested) const
    {
        if (!context.isBehindRounds)
            return tested;
        for (size_t index = path.blocks.size(); index-- > 1;) {
            size_t parameterIndex = at(path.blocks[index]).parameters.find(tested);
            if (parameterIndex == notFound)
                return tested;
            ValueID passed;
            for (const Target& target : at(path.blocks[index - 1]).terminator.targets) {
                if (target.block != path.blocks[index])
                    continue;
                if (parameterIndex >= target.arguments.size())
                    return { };
                ValueID argument = sameObjectBehindFacts(function, target.arguments[parameterIndex]);
                if (passed && passed != argument)
                    return { };
                passed = argument;
            }
            tested = passed;
        }
        return tested;
    }

    // The test of what a join takes, where it ends a block below the join. Every edge of the join leads there, but for the ways that are seldom taken, and whoever else comes to that block is seldom there too.
    // Behind the rounds a value may be read between the join and the test: `a + b + this.c` tests the sum of the first two where it is about to add the third.
    std::optional<TestBelowJoin> findTestBelowJoin(const CFG& cfg, BlockID id) const
    {
        const Block& join = at(id);
        if (join.isHandlerBlock() || join.frequency.predicted() != BlockFrequency::Normal)
            return std::nullopt;
        TestBelowJoin path;
        path.blocks.append(id);
        for (BlockID from = id; path.blocks.size() < maxBlocksFromJoinToTest; from = path.blocks.last()) {
            const Terminator& choice = at(from).terminator;
            BlockID below = blockBelow(choice);
            if (!below || path.blocks.contains(below))
                return std::nullopt;
            if (leadsToOneBlock(choice)) {
                for (uint32_t targetIndex = 0; targetIndex < choice.targets.size(); ++targetIndex) {
                    if (hasEdgeResults(function, Edge { from, targetIndex }))
                        return std::nullopt;
                }
            }
            bool isComeToOnlyFromAbove = std::ranges::all_of(cfg.incomingEdges(below), [&](Edge edge) {
                return edge.from == from || (context.isBehindRounds && isSeldomTaken(edge));
            });
            const Block& testing = at(below);
            if (!isComeToOnlyFromAbove || testing.isHandlerBlock() || hasEdgeTo(testing.terminator, below) || hasEdgeTo(testing.terminator, id))
                return std::nullopt;
            path.blocks.append(below);
            ValueID condition = testing.terminator.kind == TerminatorKind::Branch ? testing.terminator.conditionOperand() : ValueID();
            if (condition && at(condition).opcode == Opcode::TypeTest && testing.values.contains(condition)) {
                const Value& test = at(condition);
                path.parameterOfJoin = parameterOfJoinBehind(path, sameObjectBehindFacts(function, test.valueOperand()));
                if (path.parameterOfJoin && join.parameters.contains(path.parameterOfJoin)) {
                    path.test = &test;
                    return path;
                }
            }
            if (!context.isBehindRounds)
                return std::nullopt;
        }
        return std::nullopt;
    }

    // A second choice may stand between the join and the test: `shapes[i].scaled(factors[j])` finds the argument before it asks which method the shape has, and a list that is never made is read by a choice among its elements.
    // The edges whose values pass the test share one copy of the two blocks, the others keep the blocks, and the test folds in both. The walk of copySmallJoins() has noted the joins.
    PassOutcome copyJoinsAboveTests()
    {
        if (context.isBehindRounds) {
            for (BlockID id : analyses.cfg().reversePostOrder()) {
                if (blockBelow(at(id).terminator))
                    joinsThatLeadToOneBlock.append(id);
            }
        }
        if (joinsThatLeadToOneBlock.isEmpty())
            return PassOutcome::Unchanged;
        auto answerFor = [&](const TestBelowJoin& path, Edge edge) {
            const Block& join = at(targetOf(function, edge).block);
            const Type& passed = typeOf(substituteParameter(join, path.parameterOfJoin, targetOf(function, edge).arguments));
            return evaluateTypeTest(program, passed, context.types.at(path.test->testedType()));
        };
        bool hasCandidates = chooseMergePointsAmong(joinsThatLeadToOneBlock.span(), [&](const CFG& cfg, BlockID id) {
            std::optional<TestBelowJoin> path = findTestBelowJoin(cfg, id);
            if (!path || isLoopHeader(cfg, id))
                return false;
            if (context.isBehindRounds && heatClassOf(context, id).predicted() < leastHeatOfJoinsCopiedAboveTests)
                return false;
            bool somePass = false;
            bool someFail = false;
            for (Edge edge : cfg.incomingEdges(id)) {
                KnownBoolean answer = canRedirect(edge) ? answerFor(*path, edge) : KnownBoolean::Unknown;
                // Behind the rounds whoever keeps the blocks is seldom there, so what usually runs holds one copy of them as before.
                if (context.isBehindRounds ? answer != KnownBoolean::True && !isSeldomTaken(edge) : answer == KnownBoolean::Unknown)
                    return false;
                (answer == KnownBoolean::True ? somePass : someFail) = true;
            }
            if (!somePass || !someFail || RegionCloner::checkCanDuplicate(analyses, path->blocks.span(), singleElementSpan(id)) != CloneCheck::CanClone)
                return false;
            size_t numberOfValues = 0;
            for (BlockID block : path->blocks)
                numberOfValues += at(block).values.size();
            return !context.exceeds(LimitID::MaxThreadedBlockValues, numberOfValues, at(path->blocks.last()).terminator.conditionOperand());
        });
        if (!hasCandidates)
            return PassOutcome::Unchanged;

        // A copy adds edges to what lies below it. Whoever stands there waits for the next run.
        IndexSet<BlockID> changedBlocks;
        for (BlockID candidate : candidates) {
            std::optional<TestBelowJoin> path = findTestBelowJoin(analyses.cfg(), candidate);
            if (!path)
                continue;
            const Terminator& branch = at(path->blocks.last()).terminator;
            Vector<BlockID, 8> touchedBlocks { branch.taken().block, branch.notTaken().block };
            for (BlockID block : path->blocks) {
                touchedBlocks.append(block);
                for (const Target& target : at(block).terminator.targets)
                    touchedBlocks.append(target.block);
            }
            if (std::ranges::any_of(touchedBlocks, [&](BlockID block) { return changedBlocks.contains(block); }))
                continue;
            for (BlockID block : touchedBlocks)
                changedBlocks.add(block);

            Vector<Edge, 4> passingEdges;
            for (Edge edge : incomingEdges[candidate]) {
                if (answerFor(*path, edge) == KnownBoolean::True)
                    passingEdges.append(edge);
            }
            BlockID copy = copyBlocks(path->blocks.span());
            for (Edge edge : passingEdges)
                redirect(edge, Target { copy, Arguments(targetOf(function, edge).arguments), { } });
            if (context.isBehindRounds) {
                context.hasCopiedJoinsAboveTests = true;
                for (BlockID block : path->blocks)
                    builder().function().edit(block, BlockEdit::Rest).frequency = BlockFrequency::Rare;
            }
        }
        return finish();
    }

    struct TestBelowHeadOfLoop {
        Vector<BlockID, maxBlocksFromJoinToTest> blocks;
        Vector<Edge, 2> edgesThatPass;
    };

    ValueID valueHandedOnAs(BlockID above, BlockID block, ValueID value) const
    {
        size_t parameterIndex = at(block).parameters.find(value);
        if (parameterIndex == notFound)
            return value;
        const auto& targets = at(above).terminator.targets;
        for (uint32_t targetIndex = 0; targetIndex < targets.size(); ++targetIndex) {
            if (targets[targetIndex].block != block)
                continue;
            if (parameterIndex < targets[targetIndex].arguments.size())
                return targets[targetIndex].arguments[parameterIndex];
            return operandNarrowedOnEdge(function, { above, targetIndex, static_cast<uint32_t>(parameterIndex - firstEdgeResultParameter(function, above, targetIndex)) });
        }
        return { };
    }

    ValueID parameterOfHeadBehind(std::span<const BlockID> blocks, ValueID tested) const
    {
        tested = sameObjectBehindFacts(function, tested);
        for (size_t index = blocks.size(); index-- > 1;) {
            tested = valueHandedOnAs(blocks[index - 1], blocks[index], tested);
            if (!tested)
                return { };
            tested = sameObjectBehindFacts(function, tested);
        }
        return at(blocks[0]).parameters.contains(tested) ? tested : ValueID();
    }

    std::optional<TestBelowHeadOfLoop> findTestBelowHeadOf(const NaturalLoop& loop)
    {
        const CFG& cfg = analyses.cfg();
        const NaturalLoops& loops = analyses.naturalLoops();
        BlockID head = loop.header();
        TestBelowHeadOfLoop path;
        for (BlockID id = head; path.blocks.size() < maxBlocksFromJoinToTest;) {
            const Block& block = at(id);
            if (block.isHandlerBlock() || block.frequency.predicted() != BlockFrequency::Normal)
                return std::nullopt;
            path.blocks.append(id);

            const Value* test = typeTestBranchedOn(function, block);
            bool isOpen = test && block.values.contains(block.terminator.conditionOperand()) && evaluateTypeTest(program, typeOf(test->valueOperand()), context.types.at(test->testedType())) == KnownBoolean::Unknown;
            ValueID parameter = isOpen ? parameterOfHeadBehind(path.blocks.span(), test->valueOperand()) : ValueID();
            BlockID whenPassed = parameter ? block.terminator.taken().block : BlockID();
            if (whenPassed && whenPassed != head && loops.belongsTo(whenPassed, loop)) {
                size_t parameterIndex = at(head).parameters.find(parameter);
                for (Edge edge : cfg.incomingEdges(head)) {
                    if (!loops.belongsTo(edge.from, loop) || !canRedirect(edge) || !analyses.dominators().dominates(whenPassed, edge.from))
                        continue;
                    Type arriving = typeOf(targetOf(function, edge).arguments[parameterIndex]).castTo(typeOf(test->valueOperand()), context.types);
                    if (evaluateTypeTest(program, arriving, context.types.at(test->testedType())) == KnownBoolean::True)
                        path.edgesThatPass.append(edge);
                }
                if (!path.edgesThatPass.isEmpty())
                    return path;
            }

            BlockID below;
            for (const Target& target : block.terminator.targets) {
                if (!loops.belongsTo(target.block, loop))
                    continue;
                if (below)
                    return std::nullopt;
                below = target.block;
            }
            if (!below || below == head || !cfg.singleIncomingEdge(below))
                return std::nullopt;
            id = below;
        }
        return std::nullopt;
    }

    PassOutcome copyHeadsOfLoopsAboveTests()
    {
        const NaturalLoops& loops = analyses.naturalLoops();
        Vector<TestBelowHeadOfLoop, 4> paths;
        IndexSet<BlockID> changedBlocks;
        for (unsigned index = 0; index < loops.numLoops(); ++index) {
            BlockID head = loops.loop(index).header();
            if (heatClassOf(context, head).predicted() < leastHeatOfJoinsCopiedAboveTests)
                continue;
            std::optional<TestBelowHeadOfLoop> path = findTestBelowHeadOf(loops.loop(index));
            if (!path || RegionCloner::checkCanDuplicate(analyses, path->blocks.span(), singleElementSpan(head)) != CloneCheck::CanClone)
                continue;
            size_t numberOfValues = 0;
            Vector<BlockID, 16> touchedBlocks;
            for (BlockID block : path->blocks) {
                numberOfValues += at(block).values.size();
                touchedBlocks.append(block);
                for (const Target& target : at(block).terminator.targets)
                    touchedBlocks.append(target.block);
            }
            for (Edge edge : path->edgesThatPass)
                touchedBlocks.append(edge.from);
            if (context.exceeds(LimitID::MaxThreadedBlockValues, numberOfValues, at(path->blocks.last()).terminator.conditionOperand()))
                continue;
            if (std::ranges::any_of(touchedBlocks, [&](BlockID block) { return changedBlocks.contains(block); }))
                continue;
            for (BlockID block : touchedBlocks)
                changedBlocks.add(block);
            paths.append(WTF::move(*path));
        }
        for (const TestBelowHeadOfLoop& path : paths) {
            BlockID copy = copyBlocks(path.blocks.span());
            for (Edge edge : path.edgesThatPass)
                redirect(edge, Target { copy, Arguments(targetOf(function, edge).arguments), { } });
            context.hasCopiedJoinsAboveTests = true;
        }
        return finish();
    }

    // Two ways part at a test of a value for a type, come together again, and the same test of the same value follows: `f(f(x))` where f may be one of the functions that its type lists or any other.
    PassOutcome copyRepeatedTests()
    {
        Vector<BranchOnTypeTest> tests;
        for (BlockID block : blocks()) {
            if (const Value* test = findFunctionTestBranchedOn(context, at(block)))
                tests.append({ test->valueOperand(), test->testedType(), block });
        }
        keepRepeatedTests(analyses, tests);
        if (tests.isEmpty())
            return PassOutcome::Unchanged;
        IndexMap<BlockID, uint32_t> positionPlusOneOfBlock(function.blocks.size());
        for (uint32_t position = 0; position < tests.size(); ++position)
            positionPlusOneOfBlock[tests[position].block] = position + 1;

        bool hasCandidates = chooseMergePoints([&](const CFG& cfg, BlockID id) {
            const Block& block = at(id);
            if (isLoopHeader(cfg, id) || !positionPlusOneOfBlock[id])
                return false;
            ValueID condition = block.terminator.conditionOperand();
            if (block.isHandlerBlock() || !block.values.contains(condition) || context.exceeds(LimitID::MaxThreadedBlockValues, block.values.size(), condition))
                return false;
            bool hasOtherCode = std::ranges::any_of(block.values, [&](ValueID value) {
                return value != condition && estimatedCodeBytes(program, function, at(value));
            });
            return !hasOtherCode && !hasEdgeTo(block.terminator, id) && RegionCloner::checkCanDuplicate(analyses, singleElementSpan(id)) == CloneCheck::CanClone;
        });
        if (!hasCandidates)
            return PassOutcome::Unchanged;

        // An edge into a candidate is decided where it comes from a block that lies below one of the two edges of another test like the candidate's.
        struct IncomingEdge {
            Edge edge;
            uint32_t position;
            uint32_t nextFromSameBlockPlusOne;
        };
        Vector<IncomingEdge> edgesIntoCandidates;
        IndexMap<BlockID, uint32_t> firstEdgePlusOneFromBlock(function.blocks.size());
        for (BlockID candidate : candidates) {
            for (Edge edge : incomingEdges[candidate]) {
                edgesIntoCandidates.append({ edge, positionPlusOneOfBlock[candidate] - 1, firstEdgePlusOneFromBlock[edge.from] });
                firstEdgePlusOneFromBlock[edge.from] = edgesIntoCandidates.size();
            }
        }
        Vector<uint32_t> firstTestLike(tests.size());
        IndexMap<BlockID, uint32_t> positionPlusOneOfTestAbove(function.blocks.size());
        CFG& cfg = analyses.cfg();
        for (uint32_t position = 0; position < tests.size(); ++position) {
            firstTestLike[position] = position && tests[position - 1].test() == tests[position].test() ? firstTestLike[position - 1] : position;
            for (uint32_t targetIndex : { 0u, 1u }) {
                Edge edge { tests[position].block, targetIndex };
                BlockID target = targetOf(function, edge).block;
                if (cfg.isReachable(edge.from) && cfg.singleIncomingEdge(target) == edge)
                    positionPlusOneOfTestAbove[target] = position + 1;
            }
        }
        Vector<uint32_t> numberOfEdgesAboveOfTest(FillWith { }, tests.size(), 0u);
        Vector<uint32_t> numberOfEdgesAboveOfTestsLike(FillWith { }, tests.size(), 0u);
        BitVector isDecided(edgesIntoCandidates.size());
        analyses.dominators().walkTree([&](BlockID block) {
            if (uint32_t positionPlusOne = positionPlusOneOfTestAbove[block]) {
                ++numberOfEdgesAboveOfTest[positionPlusOne - 1];
                ++numberOfEdgesAboveOfTestsLike[firstTestLike[positionPlusOne - 1]];
            }
            for (uint32_t indexPlusOne = firstEdgePlusOneFromBlock[block]; indexPlusOne; indexPlusOne = edgesIntoCandidates[indexPlusOne - 1].nextFromSameBlockPlusOne) {
                uint32_t position = edgesIntoCandidates[indexPlusOne - 1].position;
                if (numberOfEdgesAboveOfTestsLike[firstTestLike[position]] > numberOfEdgesAboveOfTest[position])
                    isDecided.quickSet(indexPlusOne - 1);
            }
        }, [&](BlockID block) {
            if (uint32_t positionPlusOne = positionPlusOneOfTestAbove[block]) {
                --numberOfEdgesAboveOfTest[positionPlusOne - 1];
                --numberOfEdgesAboveOfTestsLike[firstTestLike[positionPlusOne - 1]];
            }
        });

        Vector<Edge, 8> decidedEdges;
        for (size_t index : isDecided)
            decidedEdges.append(edgesIntoCandidates[index].edge);
        decideTypeTestsOf(copyTargetsOf(decidedEdges.span()).span());
        return finish();
    }

    Vector<BlockID> candidates;
    Vector<BlockID, 4> joinsThatLeadToOneBlock;
    IndexSet<BlockID> isCandidate;
    IndexMap<BlockID, Vector<Edge, 2>> incomingEdges;
    std::optional<SSARepair> repair;
    Vector<SSARepair::Variable, 8> variables;
    UncheckedKeyHashMap<uint32_t, size_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> firstVariableOfBlock;
    PassOutcome outcome { PassOutcome::Unchanged };
};

} // anonymous namespace

PassOutcome threadJumps(FunctionPassContext& context)
{
    // The rounds have left nothing for the other phases.
    if (context.isBehindRounds) {
        PassOutcome outcome = PassOutcome::Unchanged;
        if (context.inputs.heat.of(context.program, context.id).predicted() >= leastHeatOfJoinsCopiedBesideSlowPaths)
            outcome = EdgeThreader(context).copySmallJoins();
        outcome |= EdgeThreader(context).copyJoinsAboveTests();
        outcome |= EdgeThreader(context).copyHeadsOfLoopsAboveTests();
        if (outcome == PassOutcome::Changed)
            context.keptAnalyses = allAnalyses;
        return outcome;
    }
    PassOutcome outcome = EdgeThreader(context).threadBranchesOnParameters();
    outcome |= EdgeThreader(context).threadDecidedBranches();
    outcome |= EdgeThreader(context).sendUnlikelyWaysPastTestsOfMembers();
    EdgeThreader joinCopier(context);
    PassOutcome copiedSmallJoins = joinCopier.copySmallJoins();
    // A phase that has chosen no merge point has kept nothing but what it noted for the next one. Where it has copied a block, that is out of date, and the pass runs again.
    outcome |= copiedSmallJoins == PassOutcome::Unchanged ? joinCopier.copyJoinsAboveTests() : copiedSmallJoins;
    if (hasTwoFunctionTestsForOneType(context))
        outcome |= EdgeThreader(context).copyRepeatedTests();
    if (outcome == PassOutcome::Changed)
        context.keptAnalyses = allAnalyses;
    return outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
