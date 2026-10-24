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
#include "AOTKnownValue.h"
#include "AOTRegionCloner.h"
#include "AOTRewrite.h"
#include "AOTSSARepair.h"
#include <algorithm>
#include <optional>
#include <tuple>
#include <utility>
#include <wtf/IndexMap.h>
#include <wtf/IndexSet.h>

namespace JSC { namespace AOT { namespace Typed {

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
        const CFG& cfg = analyses.cfg();
        for (BlockID block : cfg.reversePostOrder()) {
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
        builder().redirectEdge(edge, WTF::move(target));
        didAddEdge(edge);
        outcome = PassOutcome::Changed;
    }

    // Each of the copies is one more definition of what the original beside it stands for. The variables of one block follow each other, and the first copy of the block makes them.
    void addDefinitions(BlockID block, const Arguments& originals, const Arguments& copies)
    {
        if (!repair)
            repair.emplace(builder(), analyses);
        size_t first = std::ranges::find(variablesByBlock, block, &std::pair<BlockID, SSARepair::Variable>::first) - variablesByBlock.begin();
        for (unsigned index = 0; index < originals.size(); ++index) {
            if (first + index == variablesByBlock.size())
                variablesByBlock.append({ block, repair->addVariable(originals[index]) });
            repair->addDefinition(variablesByBlock[first + index].second, copies[index]);
        }
    }

    BlockID copyBlock(BlockID original)
    {
        RegionCloner cloner(builder(), function);
        cloner.cloneBlocks(singleElementSpan(original), [](ValueID, Value&) { });

        Arguments definitions = at(original).parameters;
        for (ValueID value : at(original).values) {
            if (resultRuleOf(at(value).opcode) != ResultRule::None)
                definitions.append(value);
        }
        Arguments copies;
        for (ValueID definition : definitions)
            copies.append(cloner.cloneOf(definition));
        addDefinitions(original, definitions, copies);
        return cloner.cloneOf(original);
    }

    PassOutcome finish()
    {
        if (repair) {
            builder().emptyUnreachableBlocks();
            repair->run();
        }
        return outcome;
    }

    // An edge that decides the test of a merge point gets a copy of the block for itself. There the test folds.
    PassOutcome giveCopiesTo(std::span<const Edge> decidedEdges)
    {
        for (Edge edge : decidedEdges) {
            BlockID copy = copyBlock(targetOf(function, edge).block);
            redirect(edge, Target { copy, Arguments(targetOf(function, edge).arguments), { } });
        }
        return finish();
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
        const UseLists& useListsBeforeCopies = analyses.useLists();
        Vector<std::tuple<Use, ValueID, ProgramPoint>, 8> usesOutsideBlocks;
        for (BlockID candidate : candidates) {
            for (ValueID id : at(candidate).values) {
                if (!isRematerializable(at(id)))
                    continue;
                for (const Use& use : useListsBeforeCopies.usesOf(id)) {
                    bool hasPointInBlock = use.kind == Use::Kind::Operand || use.kind == Use::Kind::TerminatorOperand || use.kind == Use::Kind::EdgeArgument;
                    if (use.block != candidate && hasPointInBlock)
                        usesOutsideBlocks.append({ use, id, useListsBeforeCopies.pointOf(use) });
                }
            }
        }
        // The first insertion ends the life of the use lists.
        for (const auto& [use, constant, point] : usesOutsideBlocks)
            replaceUse(use, insertBefore(point, at(constant).origin(), Value(at(constant))));
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
        auto addUserOutsideBlock = [&](BlockID candidate, ValueID definition, BlockID user) {
            if (user == candidate)
                return;
            usersOutsideBlock[candidate].add(user);
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
                if (*usersBelow == UsersBelow::Some)
                    skippingEdges.append({ edge, *targetIndex });
                std::optional<Target> target = *usersBelow == UsersBelow::None ? createThreadedTarget(mergePoint, chosenEdge, incomingArguments) : std::nullopt;
                if (!target)
                    continue;
                for (ValueID argument : target->arguments) {
                    if (BlockID definingBlock = definingCandidate[argument])
                        addUserOutsideBlock(definingBlock, argument, edge.from);
                }
                redirect(edge, WTF::move(*target));
            }
        }

        // The edge goes through a block of its own with the parameters of the block that it skips: each is one more definition of what the parameter stands for, and the joins below get parameters for the two.
        for (auto [edge, targetIndex] : skippingEdges) {
            BlockID mergePoint = targetOf(function, edge).block;
            Edge chosenEdge { mergePoint, targetIndex };
            // An edge that skips the block has a value for each of its parameters. What the block computes exists only behind it.
            if (!isCandidate.contains(mergePoint) || hasHandlers || computesValuesForOthers.contains(mergePoint))
                continue;
            Arguments incomingArguments = targetOf(function, edge).arguments;
            if (!createThreadedTarget(at(mergePoint), chosenEdge, incomingArguments))
                continue;

            BlockID target = targetOf(function, chosenEdge).block;
            Arguments argumentsOfChosenEdge = argumentsWithoutTest(function, chosenEdge);
            builder().setOrigin(at(mergePoint).terminator.origin());
            BlockID skippingBlock = builder().addBlock(at(edge.from).frequency);
            Arguments parameters;
            for (ValueID argument : incomingArguments)
                parameters.append(builder().addParameter(skippingBlock, at(argument).type));
            Target next = substituteParameters(at(mergePoint), target, argumentsOfChosenEdge, parameters);
            builder().moveToEndOf(skippingBlock);
            builder().emitJump(next.block, WTF::move(next.arguments));
            redirect(edge, Target { skippingBlock, WTF::move(incomingArguments), { } });
            addDefinitions(mergePoint, at(mergePoint).parameters, parameters);
        }
        return finish();
    }

    // What an edge passes decides the test, but the block does more than test: it reads a variable that the code below uses, or may throw. No edge can skip that, and a copy of it costs less than the test and the merge.
    PassOutcome copySmallJoins()
    {
        bool hasCandidates = chooseMergePoints([&](const CFG& cfg, BlockID id) {
            const Block& block = at(id);
            if (isLoopHeader(cfg, id) || block.terminator.kind != TerminatorKind::Branch || block.isHandlerBlock() || hasEdgeTo(block.terminator, id) || block.frequency != BlockFrequency::Normal)
                return false;
            ValueID condition = block.terminator.conditionOperand();
            const Value& test = at(condition);
            if (test.opcode != Opcode::TypeTest || !block.values.contains(condition) || !block.parameters.contains(test.valueOperand()))
                return false;
            size_t numberOfCopies = cfg.incomingEdges(id).size() - 1;
            if (context.exceeds(LimitID::MaxThreadedBlockValues, block.values.size() * numberOfCopies, condition))
                return false;
            if (RegionCloner::checkCanDuplicate(analyses, singleElementSpan(id)) != CloneCheck::CanClone)
                return false;
            const Type& tested = context.types.at(test.testedType());
            return std::ranges::all_of(cfg.incomingEdges(id), [&](Edge edge) {
                if (!canRedirect(edge))
                    return false;
                const Type& passed = typeOf(substituteParameter(block, test.valueOperand(), targetOf(function, edge).arguments));
                return evaluateTypeTest(program, passed, tested) != KnownBoolean::Unknown;
            });
        });
        if (!hasCandidates)
            return PassOutcome::Unchanged;

        // The last edge keeps the block.
        Vector<Edge, 8> decidedEdges;
        for (BlockID candidate : candidates)
            decidedEdges.append(incomingEdges[candidate].span().first(incomingEdges[candidate].size() - 1));
        return giveCopiesTo(decidedEdges.span());
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

        Vector<Edge, 8> decidedEdges;
        for (BlockID candidate : candidates) {
            auto testsLikeIt = std::ranges::equal_range(tests, tests[positionPlusOneOfBlock[candidate] - 1].test(), { }, &BranchOnTypeTest::test);
            for (Edge edge : incomingEdges[candidate]) {
                bool isDecided = std::ranges::any_of(testsLikeIt, [&](const BranchOnTypeTest& earlier) {
                    if (earlier.block == candidate)
                        return false;
                    return analyses.dominates(Edge { earlier.block, 0 }, edge.from) || analyses.dominates(Edge { earlier.block, 1 }, edge.from);
                });
                if (isDecided)
                    decidedEdges.append(edge);
            }
        }
        return giveCopiesTo(decidedEdges.span());
    }

    Vector<BlockID> candidates;
    IndexSet<BlockID> isCandidate;
    IndexMap<BlockID, Vector<Edge, 2>> incomingEdges;
    std::optional<SSARepair> repair;
    Vector<std::pair<BlockID, SSARepair::Variable>, 8> variablesByBlock;
    PassOutcome outcome { PassOutcome::Unchanged };
};

} // anonymous namespace

PassOutcome threadJumps(FunctionPassContext& context)
{
    PassOutcome outcome = EdgeThreader(context).threadBranchesOnParameters();
    outcome |= EdgeThreader(context).threadDecidedBranches();
    outcome |= EdgeThreader(context).copySmallJoins();
    if (hasTwoFunctionTestsForOneType(context))
        outcome |= EdgeThreader(context).copyRepeatedTests();
    if (outcome == PassOutcome::Changed)
        context.keptAnalyses = allAnalyses;
    return outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
