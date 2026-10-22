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
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTKnownValue.h"
#include "AOTRegionCloner.h"
#include "AOTSSARepair.h"
#include "AOTValueSubstitution.h"
#include <algorithm>
#include <optional>
#include <utility>
#include <wtf/IndexMap.h>
#include <wtf/IndexSet.h>
#include <wtf/Vector.h>

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

static ValueID testedValueOf(const Terminator& terminator)
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
    return terminator.targets.containsIf([&](const Target& target) {
        return target.block == block;
    });
}

static bool isMergePoint(const CFG& cfg, BlockID block)
{
    return cfg.incomingEdges(block).size() >= 2;
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

static bool passesValuesOf(const Block& block, const Arguments& passed)
{
    return passed.containsIf([&](ValueID argument) {
        return block.values.contains(argument);
    });
}

namespace {

class EdgeThreader {
public:
    explicit EdgeThreader(FunctionPassContext& context)
        : m_context(context)
        , m_program(context.program)
        , m_function(context.function)
        , m_analyses(context.analyses)
        , m_builder(context.analyses.context(), context.function, context.types, &context.analyses)
    {
    }

protected:
    bool canRedirect(Edge edge) const
    {
        if (edge.isHandlerEdge() || hasEdgeResults(m_function, edge))
            return false;
        return edge.from != targetOf(m_function, edge).block && hasRedirectableEdges(m_function.at(edge.from).terminator.kind);
    }

    void collectIncomingEdges()
    {
        const CFG& cfg = m_analyses.cfg();
        m_incomingEdges = IndexMap<BlockID, Vector<Edge, 2>>(m_function.blocks.size());
        for (BlockID candidate : m_candidates) {
            m_isCandidate.add(candidate);
            for (Edge edge : cfg.incomingEdges(candidate)) {
                if (canRedirect(edge))
                    m_incomingEdges[candidate].append(edge);
            }
        }
    }

    void didAddEdge(Edge edge)
    {
        BlockID target = targetOf(m_function, edge).block;
        if (m_isCandidate.contains(target) && canRedirect(edge))
            m_incomingEdges[target].append(edge);
    }

    std::optional<Target> createThreadedTarget(const Block& mergePoint, Edge chosenEdge, const Arguments& incomingArguments) const
    {
        Target target = substituteParameters(mergePoint, targetOf(m_function, chosenEdge).block, argumentsWithoutTest(m_function, chosenEdge), incomingArguments);
        if (hasEdgeResults(m_function, chosenEdge) && !argumentsFitParameters(m_program, m_function, target))
            return std::nullopt;
        return target;
    }

    void redirect(Edge edge, Target&& target)
    {
        m_builder.redirectEdge(edge, WTF::move(target));
        didAddEdge(edge);
        m_outcome = PassOutcome::Changed;
    }

    FunctionPassContext& m_context;
    const Program& m_program;
    Function& m_function;
    FunctionAnalyses& m_analyses;
    IRBuilder m_builder;
    Vector<BlockID> m_candidates;
    IndexSet<BlockID> m_isCandidate;
    IndexMap<BlockID, Vector<Edge, 2>> m_incomingEdges;
    PassOutcome m_outcome { PassOutcome::Unchanged };
};

class ParameterBranchThreader : public EdgeThreader {
public:
    using EdgeThreader::EdgeThreader;

    PassOutcome run()
    {
        findCandidates();
        if (m_candidates.isEmpty())
            return PassOutcome::Unchanged;
        collectIncomingEdges();
        findLoopHeadersToKeep();
        for (BlockID candidate : m_candidates) {
            if (m_copiedCandidates.contains(candidate))
                threadEdgesThroughCopies(candidate);
            else
                threadEdgesInto(candidate);
        }
        mergeCopiedDefinitions();
        return m_outcome;
    }

private:
    struct CopiedMergePoint {
        BlockID original;
        uint32_t targetIndex { 0 };
        BlockID copy;
        Arguments originalDefinitions;
        Arguments copiedDefinitions;
    };

    struct DecidedEdge {
        Edge edge;
        uint32_t targetIndex { 0 };
    };

    void findCandidates()
    {
        const CFG& cfg = m_analyses.cfg();
        for (BlockID block : cfg.reversePostOrder()) {
            if (isMergePoint(cfg, block) && isCandidate(block))
                m_candidates.append(block);
        }
    }

    bool diesInBlock(ValueID definition, BlockID block)
    {
        return std::ranges::all_of(m_analyses.useLists().usesOf(definition), [&](const Use& use) {
            return use.block == block;
        });
    }

    bool isCandidate(BlockID id)
    {
        const Block& block = m_function.at(id);
        ValueID tested = testedValueOf(block.terminator);
        if (!tested || block.isHandlerBlock() || !block.parameters.contains(tested))
            return false;
        if (m_analyses.useLists().useCount(tested) != 1)
            return false;
        bool parametersDieInBlock = std::ranges::all_of(block.parameters, [&](ValueID parameter) {
            return diesInBlock(parameter, id);
        });
        if (parametersDieInBlock && block.values.isEmpty())
            return true;

        // The dispatch of a state machine carries every live value. Skipping it is worth a copy of the block for each state, as long as a copy has no code.
        if (block.terminator.kind != TerminatorKind::Switch || !canCopyWithoutCode(id))
            return false;
        m_copiedCandidates.add(id);
        return true;
    }

    bool canCopyWithoutCode(BlockID id)
    {
        bool hasCode = std::ranges::any_of(m_function.at(id).values, [&](ValueID value) {
            return estimatedCodeBytes(m_program, m_function, m_function.at(value));
        });
        return !hasCode && RegionCloner::checkCanDuplicate(m_analyses, singleElementSpan(id)) == CloneCheck::CanClone;
    }

    Vector<DecidedEdge, 2> findDecidedEdges(BlockID candidate) const
    {
        const Block& mergePoint = m_function.at(candidate);
        size_t testedIndex = mergePoint.parameters.find(testedValueOf(mergePoint.terminator));
        Vector<DecidedEdge, 2> decidedEdges;
        for (Edge edge : m_incomingEdges[candidate]) {
            KnownValue known = knownValueOf(m_program, m_function, targetOf(m_function, edge).arguments[testedIndex]);
            std::optional<uint32_t> targetIndex = decidedTargetOf(mergePoint.terminator, known);
            if (targetIndex && !hasEdgeResults(m_function, Edge { candidate, *targetIndex }))
                decidedEdges.append({ edge, *targetIndex });
        }
        return decidedEdges;
    }

    // Decided before the first edge moves: a block that every edge has skipped is unreachable and still lists its own edges.
    void findLoopHeadersToKeep()
    {
        const CFG& cfg = m_analyses.cfg();
        for (BlockID candidate : m_candidates) {
            if (m_copiedCandidates.contains(candidate) && isLoopHeader(cfg, candidate) && !leavesLoopWithOneEntry(cfg, candidate))
                m_keptLoopHeaders.add(candidate);
        }
    }

    // An edge that skips the header of a loop enters the loop at a second block, unless the header goes away and every edge from outside takes the same way in.
    bool leavesLoopWithOneEntry(const CFG& cfg, BlockID header) const
    {
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
    }

    void threadEdgesThroughCopies(BlockID candidate)
    {
        if (m_keptLoopHeaders.contains(candidate) || hasEdgeTo(m_function.at(candidate).terminator, candidate))
            return;

        for (auto [edge, targetIndex] : findDecidedEdges(candidate)) {
            Arguments arguments = targetOf(m_function, edge).arguments;
            BlockID copy = ensureCopy(candidate, targetIndex);
            redirect(edge, Target { copy, WTF::move(arguments), { } });
        }
    }

    BlockID ensureCopy(BlockID original, uint32_t targetIndex)
    {
        for (const CopiedMergePoint& existing : m_copies) {
            if (existing.original == original && existing.targetIndex == targetIndex)
                return existing.copy;
        }

        RegionCloner cloner(m_builder, m_function);
        cloner.cloneBlocks(singleElementSpan(original), [](ValueID, Value&) { });

        CopiedMergePoint copied { original, targetIndex, cloner.cloneOf(original), { }, { } };
        auto addDefinition = [&](ValueID definition) {
            copied.originalDefinitions.append(definition);
            copied.copiedDefinitions.append(cloner.cloneOf(definition));
        };
        for (ValueID parameter : m_function.at(original).parameters)
            addDefinition(parameter);
        for (ValueID value : m_function.at(original).values) {
            if (resultRuleOf(m_function.at(value).opcode) != ResultRule::None)
                addDefinition(value);
        }

        Terminator dispatch = std::exchange(m_function.edit(copied.copy).terminator, Terminator { });
        Target& chosen = dispatch.targets[targetIndex];
        m_builder.setOrigin(dispatch.origin());
        m_builder.moveToEndOf(copied.copy);
        m_builder.emitJump(chosen.block, WTF::move(chosen.arguments));
        // The builder reports the jump, not the terminator in whose place it stands.
        m_analyses.didChangeControlFlow();
        didAddEdge(Edge { copied.copy, 0 });

        BlockID copy = copied.copy;
        m_copies.append(WTF::move(copied));
        return copy;
    }

    void mergeCopiedDefinitions()
    {
        if (m_copies.isEmpty())
            return;

        m_builder.emptyUnreachableBlocks();
        SSARepair repair(m_builder, m_analyses);
        for (size_t first = 0; first < m_copies.size(); ++first) {
            const CopiedMergePoint& firstCopy = m_copies[first];
            bool isFirstCopyOfOriginal = std::ranges::none_of(m_copies.span().first(first), [&](const CopiedMergePoint& earlier) {
                return earlier.original == firstCopy.original;
            });
            if (!isFirstCopyOfOriginal)
                continue;
            for (size_t index = 0; index < firstCopy.originalDefinitions.size(); ++index) {
                SSARepair::Variable variable = repair.addVariable(firstCopy.originalDefinitions[index]);
                for (const CopiedMergePoint& copied : m_copies.span().subspan(first)) {
                    if (copied.original == firstCopy.original)
                        repair.addDefinition(variable, copied.copiedDefinitions[index]);
                }
            }
        }
        repair.run();
    }

    void threadEdgesInto(BlockID candidate)
    {
        const Block& mergePoint = m_function.at(candidate);
        const Terminator& terminator = mergePoint.terminator;
        if (hasEdgeTo(terminator, candidate))
            return;
        ValueID tested = testedValueOf(terminator);
        for (Edge edge : m_incomingEdges[candidate]) {
            const Arguments& incomingArguments = targetOf(m_function, edge).arguments;
            KnownValue known = knownValueOf(m_program, m_function, substituteParameter(mergePoint, tested, incomingArguments));
            if (std::optional<uint32_t> targetIndex = decidedTargetOf(terminator, known))
                threadEdge(edge, mergePoint, Edge { candidate, *targetIndex });
            else if (terminator.kind == TerminatorKind::Branch && m_function.at(edge.from).terminator.kind == TerminatorKind::Jump)
                replaceJumpWithBranch(edge.from, mergePoint);
        }
    }

    void threadEdge(Edge edge, const Block& mergePoint, Edge chosenEdge)
    {
        if (!canFollowWithoutTest(m_function, chosenEdge))
            return;
        if (std::optional<Target> target = createThreadedTarget(mergePoint, chosenEdge, targetOf(m_function, edge).arguments))
            redirect(edge, WTF::move(*target));
    }

    void replaceJumpWithBranch(BlockID source, const Block& mergePoint)
    {
        const Terminator& branch = mergePoint.terminator;
        const Terminator& jump = m_function.at(source).terminator;
        const Arguments& incomingArguments = jump.next().arguments;
        Origin origin = jump.origin();
        ValueID condition = substituteParameter(mergePoint, branch.conditionOperand(), incomingArguments);
        Target taken = substituteParameters(mergePoint, branch.taken().block, branch.taken().arguments, incomingArguments);
        Target notTaken = substituteParameters(mergePoint, branch.notTaken().block, branch.notTaken().arguments, incomingArguments);

        m_function.edit(source).terminator = Terminator { };
        m_builder.setOrigin(origin);
        m_builder.moveToEndOf(source);
        m_builder.emitBranch(condition, WTF::move(taken), WTF::move(notTaken));
        // The builder reports the branch, not the terminator in whose place it stands.
        m_analyses.didChangeControlFlow();
        didAddEdge(Edge { source, 0 });
        didAddEdge(Edge { source, 1 });
        m_outcome = PassOutcome::Changed;
    }

    IndexSet<BlockID> m_copiedCandidates;
    IndexSet<BlockID> m_keptLoopHeaders;
    Vector<CopiedMergePoint, 4> m_copies;
};

class DecidedBranchThreader : public EdgeThreader {
public:
    using EdgeThreader::EdgeThreader;

    PassOutcome run()
    {
        findCandidates();
        if (m_candidates.isEmpty())
            return PassOutcome::Unchanged;
        copyConstantsToUsersOutsideBlocks();
        collectIncomingEdges();
        findUsersOutsideBlocks();
        numberDominatorTree();
        for (BlockID candidate : m_candidates)
            threadEdgesInto(candidate);
        passParametersAlongSkippingEdges();
        return m_outcome;
    }

private:
    enum class UsersBelow : uint8_t { None, Some, Unknown };

    struct SkippingEdge {
        Edge edge;
        uint32_t targetIndex { 0 };
    };

    struct PassedParameters {
        BlockID mergePoint;
        Arguments original;
        Arguments passed;
    };

    void findCandidates()
    {
        const CFG& cfg = m_analyses.cfg();
        for (BlockID block : cfg.reversePostOrder()) {
            m_hasHandlers |= m_function.at(block).isHandlerBlock();
            if (isMergePoint(cfg, block) && !isLoopHeader(cfg, block) && canSkip(m_function.at(block)) && hasDecidedEdge(cfg, block))
                m_candidates.append(block);
        }
    }

    bool canSkip(const Block& block)
    {
        ValueID tested = testedValueOf(block.terminator);
        if (!tested || block.isHandlerBlock() || block.parameters.isEmpty())
            return false;
        if (!block.parameters.contains(tested) && !block.values.contains(tested))
            return false;
        bool canRemoveValues = std::ranges::all_of(block.values, [&](ValueID value) {
            return m_analyses.isRemovableWhenUnused(value);
        });
        return canRemoveValues && !m_context.exceeds(LimitID::MaxThreadedBlockValues, block.values.size(), tested);
    }

    bool hasDecidedEdge(const CFG& cfg, BlockID candidate) const
    {
        const Block& mergePoint = m_function.at(candidate);
        return std::ranges::any_of(cfg.incomingEdges(candidate), [&](Edge edge) {
            return canRedirect(edge) && decidedTargetOf(mergePoint.terminator, evaluateBranch(mergePoint, targetOf(m_function, edge).arguments)).has_value();
        });
    }

    KnownValue evaluateBranch(const Block& mergePoint, const Arguments& incomingArguments) const
    {
        Vector<std::pair<ValueID, KnownValue>, 8> known;
        auto lookUp = [&](ValueID id) -> KnownValue {
            for (auto& [value, knownValue] : known) {
                if (value == id)
                    return knownValue;
            }
            return knownValueOf(m_program, m_function, substituteParameter(mergePoint, id, incomingArguments));
        };
        for (ValueID id : mergePoint.values) {
            KnownValue value = evaluateWithKnownOperands(m_program, m_function, m_function.at(id), lookUp);
            known.append({ id, value.isKnown() ? value : evaluateTestOfArgument(mergePoint, m_function.at(id), incomingArguments) });
        }
        return lookUp(testedValueOf(mergePoint.terminator));
    }

    // The type of what an edge passes may decide a test that the type of the parameter leaves open.
    KnownValue evaluateTestOfArgument(const Block& mergePoint, const Value& test, const Arguments& incomingArguments) const
    {
        if (test.opcode != Opcode::TypeTest && test.opcode != Opcode::IsClosureOf)
            return { };
        ValueID tested = test.valueOperand();
        if (!mergePoint.parameters.contains(tested))
            return { };
        const Type& passed = m_program.typeOf(m_function, substituteParameter(mergePoint, tested, incomingArguments));
        KnownBoolean result = test.opcode == Opcode::TypeTest
            ? evaluateTypeTest(m_program, passed, m_context.types.at(test.testedType()))
            : evaluateIsClosureOf(m_program, passed, test.function());
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

    // A constant that stands beside the test is no reason to pass through the block: it is made again where it is used.
    void copyConstantsToUsersOutsideBlocks()
    {
        struct UseOutsideBlock {
            Use use;
            ValueID constant;
            ProgramPoint point;
        };
        const UseLists& useLists = m_analyses.useLists();
        Vector<UseOutsideBlock, 8> uses;
        for (BlockID candidate : m_candidates) {
            for (ValueID id : m_function.at(candidate).values) {
                if (!isRematerializable(m_function.at(id)))
                    continue;
                for (const Use& use : useLists.usesOf(id)) {
                    bool hasPointInBlock = use.kind == Use::Kind::Operand || use.kind == Use::Kind::TerminatorOperand || use.kind == Use::Kind::EdgeArgument;
                    if (use.block != candidate && hasPointInBlock)
                        uses.append({ use, id, useLists.pointOf(use) });
                }
            }
        }
        if (uses.isEmpty())
            return;

        // The first insertion ends the life of the use lists.
        for (const UseOutsideBlock& outside : uses) {
            Value copy = m_function.at(outside.constant);
            m_builder.setOrigin(copy.origin());
            replaceUse(m_function, outside.use, m_builder.insertBefore(outside.point, WTF::move(copy)));
        }
        m_builder.applyInsertions();
        // The builder reports the copies, not the uses that now name them.
        m_analyses.didChangeValues();
        m_outcome = PassOutcome::Changed;
    }

    void findUsersOutsideBlocks()
    {
        const UseLists& useLists = m_analyses.useLists();
        m_definingCandidate = IndexMap<ValueID, BlockID>(m_function.values.size());
        m_usersOutsideBlock = IndexMap<BlockID, IndexSet<BlockID>>(m_function.blocks.size());
        for (BlockID candidate : m_candidates) {
            auto addUsersOf = [&](ValueID definition) {
                m_definingCandidate[definition] = candidate;
                for (const Use& use : useLists.usesOf(definition))
                    addUserOutsideBlock(candidate, definition, use.block);
            };
            const Block& mergePoint = m_function.at(candidate);
            for (ValueID parameter : mergePoint.parameters)
                addUsersOf(parameter);
            for (ValueID value : mergePoint.values)
                addUsersOf(value);
        }
    }

    void addUserOutsideBlock(BlockID candidate, ValueID definition, BlockID user)
    {
        if (user == candidate)
            return;
        m_usersOutsideBlock[candidate].add(user);
        if (!m_function.at(candidate).parameters.contains(definition))
            m_computesValuesForOthers.add(candidate);
    }

    void numberDominatorTree()
    {
        m_firstInSubtree = IndexMap<BlockID, uint32_t>(m_function.blocks.size());
        m_lastInSubtree = IndexMap<BlockID, uint32_t>(m_function.blocks.size());
        m_numberOfNumberedBlocks = m_function.blocks.size();
        uint32_t number = 0;
        m_analyses.dominators().walkTree([&](BlockID block) {
            m_firstInSubtree[block] = ++number;
        }, [&](BlockID block) {
            m_lastInSubtree[block] = number;
        });
    }

    // A block made since the tree was numbered counts as dominated.
    bool wasDominatedBy(BlockID candidate, BlockID block) const
    {
        if (block.index() >= m_numberOfNumberedBlocks)
            return true;
        return m_firstInSubtree[block] >= m_firstInSubtree[candidate] && m_firstInSubtree[block] <= m_lastInSubtree[candidate];
    }

    // The block dominates its users, so a way to a user that does not pass through the block leads only through what the block dominates. An edge that this pass has redirected stands for
    // a way through another candidate, which led through the same blocks.
    UsersBelow findUsersBelow(BlockID candidate, BlockID start)
    {
        const IndexSet<BlockID>& users = m_usersOutsideBlock[candidate];
        if (users.isEmpty() || !wasDominatedBy(candidate, start))
            return UsersBelow::None;
        IndexSet<BlockID> seen;
        seen.add(candidate);
        seen.add(start);
        Vector<BlockID, 16> worklist { start };
        uint64_t remainingSteps = m_context.limit(LimitID::MaxThreadingReachabilitySteps);
        while (!worklist.isEmpty()) {
            BlockID block = worklist.takeLast();
            if (users.contains(block))
                return UsersBelow::Some;
            if (!remainingSteps--) {
                m_context.gaveUp(LimitID::MaxThreadingReachabilitySteps, testedValueOf(m_function.at(candidate).terminator));
                return UsersBelow::Unknown;
            }
            m_function.at(block).forEachTarget([&](const Target& target) {
                if (wasDominatedBy(candidate, target.block) && seen.add(target.block))
                    worklist.append(target.block);
            });
        }
        return UsersBelow::None;
    }

    std::optional<UsersBelow> findUsersBelowSkippedTest(Edge chosenEdge)
    {
        if (!canFollowWithoutTest(m_function, chosenEdge))
            return std::nullopt;
        if (passesValuesOf(m_function.at(chosenEdge.from), argumentsWithoutTest(m_function, chosenEdge)))
            return std::nullopt;
        return findUsersBelow(chosenEdge.from, targetOf(m_function, chosenEdge).block);
    }

    // An edge that skips the block has a value for each of its parameters. What the block computes exists only behind it.
    bool canPassParametersAlong(BlockID mergePoint) const
    {
        return !m_hasHandlers && !m_computesValuesForOthers.contains(mergePoint);
    }

    void threadEdgesInto(BlockID candidate)
    {
        const Block& mergePoint = m_function.at(candidate);
        const Terminator& terminator = mergePoint.terminator;
        if (hasEdgeTo(terminator, candidate))
            return;
        Vector<std::optional<std::optional<UsersBelow>>, 2> usersBelowTargets(FillWith { }, terminator.targets.size(), std::nullopt);
        for (Edge edge : m_incomingEdges[candidate]) {
            const Arguments& incomingArguments = targetOf(m_function, edge).arguments;
            std::optional<uint32_t> targetIndex = decidedTargetOf(terminator, evaluateBranch(mergePoint, incomingArguments));
            if (!targetIndex)
                continue;
            Edge chosenEdge { candidate, *targetIndex };
            std::optional<std::optional<UsersBelow>>& usersBelow = usersBelowTargets[*targetIndex];
            if (!usersBelow)
                usersBelow = findUsersBelowSkippedTest(chosenEdge);
            if (!*usersBelow || **usersBelow == UsersBelow::Unknown)
                continue;
            if (**usersBelow == UsersBelow::Some) {
                m_skippingEdges.append({ edge, *targetIndex });
                continue;
            }
            std::optional<Target> target = createThreadedTarget(mergePoint, chosenEdge, incomingArguments);
            if (!target)
                continue;
            for (ValueID argument : target->arguments) {
                if (BlockID definingCandidate = m_definingCandidate[argument])
                    addUserOutsideBlock(definingCandidate, argument, edge.from);
            }
            redirect(edge, WTF::move(*target));
        }
    }

    // The edge goes through a block of its own with the parameters of the block that it skips: each is one more definition of what the parameter stands for,
    // and the joins below get parameters for the two. The block holds no code.
    void passParametersAlongSkippingEdges()
    {
        Vector<PassedParameters, 4> passed;
        for (auto [edge, targetIndex] : m_skippingEdges) {
            BlockID mergePoint = targetOf(m_function, edge).block;
            Edge chosenEdge { mergePoint, targetIndex };
            if (!m_isCandidate.contains(mergePoint) || !canPassParametersAlong(mergePoint))
                continue;
            Arguments incomingArguments = targetOf(m_function, edge).arguments;
            if (!createThreadedTarget(m_function.at(mergePoint), chosenEdge, incomingArguments))
                continue;

            BlockID target = targetOf(m_function, chosenEdge).block;
            Arguments argumentsOfChosenEdge = argumentsWithoutTest(m_function, chosenEdge);
            m_builder.setOrigin(m_function.at(mergePoint).terminator.origin());
            BlockID skippingBlock = m_builder.addBlock(m_function.at(edge.from).frequency);
            Arguments parameters;
            for (ValueID argument : incomingArguments)
                parameters.append(m_builder.addParameter(skippingBlock, m_function.at(argument).type));
            Target next = substituteParameters(m_function.at(mergePoint), target, argumentsOfChosenEdge, parameters);
            m_builder.moveToEndOf(skippingBlock);
            m_builder.emitJump(next.block, WTF::move(next.arguments));
            redirect(edge, Target { skippingBlock, WTF::move(incomingArguments), { } });
            passed.append({ mergePoint, m_function.at(mergePoint).parameters, WTF::move(parameters) });
        }
        if (passed.isEmpty())
            return;

        m_builder.emptyUnreachableBlocks();
        SSARepair repair(m_builder, m_analyses);
        for (size_t first = 0; first < passed.size(); ++first) {
            BlockID mergePoint = passed[first].mergePoint;
            bool isFirstOfMergePoint = std::ranges::none_of(passed.span().first(first), [&](const PassedParameters& earlier) {
                return earlier.mergePoint == mergePoint;
            });
            if (!isFirstOfMergePoint)
                continue;
            for (size_t index = 0; index < passed[first].original.size(); ++index) {
                SSARepair::Variable variable = repair.addVariable(passed[first].original[index]);
                for (const PassedParameters& other : passed.span().subspan(first)) {
                    if (other.mergePoint == mergePoint)
                        repair.addDefinition(variable, other.passed[index]);
                }
            }
        }
        repair.run();
    }

    IndexMap<ValueID, BlockID> m_definingCandidate;
    IndexMap<BlockID, IndexSet<BlockID>> m_usersOutsideBlock;
    IndexMap<BlockID, uint32_t> m_firstInSubtree;
    IndexMap<BlockID, uint32_t> m_lastInSubtree;
    size_t m_numberOfNumberedBlocks { 0 };
    IndexSet<BlockID> m_computesValuesForOthers;
    Vector<SkippingEdge, 4> m_skippingEdges;
    bool m_hasHandlers { false };
};

// Each run of RepeatedTestThreader takes one step along a row of equal tests, and every step costs a round of the passes beside this one. A row of calls of one function is short.
static const Value* findFunctionTestBranchedOn(const FunctionPassContext& context, const Block& block)
{
    if (block.terminator.kind != TerminatorKind::Branch)
        return nullptr;
    const Value& condition = context.function.at(block.terminator.conditionOperand());
    if (condition.opcode != Opcode::TypeTest)
        return nullptr;
    const Type& tested = context.types.at(condition.testedType());
    bool listsOnlyFunctions = !tested.functions.isEmpty() && tested.hasOnlyKnownCallees() && !tested.primitives && !tested.layouts && tested.builtins.isEmpty();
    return listsOnlyFunctions ? &condition : nullptr;
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

// Two ways part at a test of a value for a type, come together again, and the same test of the same value follows: `f(f(x))` where f may be one of the functions that its type lists or any other.
// Each way knows how the test ends. It gets a copy of the block with the test, which the first test then dominates, so that PropagateBranchConditions decides the copy,
// and what the ways computed in between is no longer merged.
class RepeatedTestThreader : public EdgeThreader {
public:
    using EdgeThreader::EdgeThreader;

    PassOutcome run()
    {
        collectTests();
        if (m_tests.isEmpty())
            return PassOutcome::Unchanged;
        findCandidates();
        if (m_candidates.isEmpty())
            return PassOutcome::Unchanged;
        collectIncomingEdges();

        Vector<DecidedEdge, 8> decidedEdges;
        for (BlockID candidate : m_candidates) {
            for (Edge edge : m_incomingEdges[candidate]) {
                if (isDecidedAt(candidate, edge.from))
                    decidedEdges.append({ candidate, edge });
            }
        }
        for (const DecidedEdge& decided : decidedEdges)
            giveCopyTo(decided);
        mergeCopiedDefinitions();
        return m_outcome;
    }

private:
    struct Test {
        ValueID subject;
        TypeID testedType;
        BlockID block;

        auto key() const { return std::tuple { subject.index(), testedType.index() }; }
    };

    struct DecidedEdge {
        BlockID mergePoint;
        Edge edge;
    };

    struct Copy {
        BlockID original;
        Arguments originalDefinitions;
        Arguments copiedDefinitions;
    };

    void collectTests()
    {
        for (BlockID block : m_analyses.cfg().reversePostOrder()) {
            if (const Value* test = findFunctionTestBranchedOn(m_context, m_function.at(block)))
                m_tests.append({ test->valueOperand(), test->testedType(), block });
        }
        // The identities of values are asked for only where two tests are for one type.
        std::ranges::stable_sort(m_tests, { }, &Test::testedType);
        Vector<Test> repeated;
        for (size_t index = 0; index < m_tests.size(); ++index) {
            bool equalsPrevious = index && m_tests[index - 1].testedType == m_tests[index].testedType;
            bool equalsNext = index + 1 < m_tests.size() && m_tests[index + 1].testedType == m_tests[index].testedType;
            if (equalsPrevious || equalsNext)
                repeated.append(m_tests[index]);
        }
        m_tests = WTF::move(repeated);
        for (Test& test : m_tests)
            test.subject = m_analyses.identityOf(test.subject);
        std::ranges::stable_sort(m_tests, { }, &Test::key);

        m_positionOfBlock = IndexMap<BlockID, uint32_t>(m_function.blocks.size());
        for (uint32_t position = 0; position < m_tests.size(); ++position)
            m_positionOfBlock[m_tests[position].block] = position + 1;
    }

    std::span<const Test> testsLike(BlockID block) const
    {
        uint32_t positionPlusOne = m_positionOfBlock[block];
        if (!positionPlusOne)
            return { };
        auto range = std::ranges::equal_range(m_tests, m_tests[positionPlusOne - 1].key(), { }, &Test::key);
        return { range.begin(), range.end() };
    }

    bool holdsOnlyTheTest(BlockID id)
    {
        const Block& block = m_function.at(id);
        ValueID condition = block.terminator.conditionOperand();
        if (block.isHandlerBlock() || !block.values.contains(condition) || m_context.exceeds(LimitID::MaxThreadedBlockValues, block.values.size(), condition))
            return false;
        bool hasOtherCode = std::ranges::any_of(block.values, [&](ValueID value) {
            return value != condition && estimatedCodeBytes(m_program, m_function, m_function.at(value));
        });
        return !hasOtherCode && !hasEdgeTo(block.terminator, id) && RegionCloner::checkCanDuplicate(m_analyses, singleElementSpan(id)) == CloneCheck::CanClone;
    }

    void findCandidates()
    {
        const CFG& cfg = m_analyses.cfg();
        for (BlockID block : cfg.reversePostOrder()) {
            if (isMergePoint(cfg, block) && !isLoopHeader(cfg, block) && testsLike(block).size() > 1 && holdsOnlyTheTest(block))
                m_candidates.append(block);
        }
    }

    bool isDecidedAt(BlockID mergePoint, BlockID predecessor)
    {
        return std::ranges::any_of(testsLike(mergePoint), [&](const Test& earlier) {
            if (earlier.block == mergePoint)
                return false;
            return m_analyses.dominates(Edge { earlier.block, 0 }, predecessor) || m_analyses.dominates(Edge { earlier.block, 1 }, predecessor);
        });
    }

    void giveCopyTo(const DecidedEdge& decided)
    {
        RegionCloner cloner(m_builder, m_function);
        cloner.cloneBlocks(singleElementSpan(decided.mergePoint), [](ValueID, Value&) { });

        Copy copy { decided.mergePoint, { }, { } };
        auto addDefinition = [&](ValueID definition) {
            copy.originalDefinitions.append(definition);
            copy.copiedDefinitions.append(cloner.cloneOf(definition));
        };
        for (ValueID parameter : m_function.at(decided.mergePoint).parameters)
            addDefinition(parameter);
        for (ValueID value : m_function.at(decided.mergePoint).values) {
            if (resultRuleOf(m_function.at(value).opcode) != ResultRule::None)
                addDefinition(value);
        }
        m_copies.append(WTF::move(copy));

        Arguments arguments = targetOf(m_function, decided.edge).arguments;
        m_builder.redirectEdge(decided.edge, Target { cloner.cloneOf(decided.mergePoint), WTF::move(arguments), { } });
        m_outcome = PassOutcome::Changed;
    }

    void mergeCopiedDefinitions()
    {
        if (m_copies.isEmpty())
            return;
        m_builder.emptyUnreachableBlocks();
        SSARepair repair(m_builder, m_analyses);
        for (size_t first = 0; first < m_copies.size(); ++first) {
            const Copy& firstCopy = m_copies[first];
            bool isFirstCopyOfOriginal = std::ranges::none_of(m_copies.span().first(first), [&](const Copy& earlier) {
                return earlier.original == firstCopy.original;
            });
            if (!isFirstCopyOfOriginal)
                continue;
            for (size_t index = 0; index < firstCopy.originalDefinitions.size(); ++index) {
                SSARepair::Variable variable = repair.addVariable(firstCopy.originalDefinitions[index]);
                for (const Copy& copy : m_copies.span().subspan(first)) {
                    if (copy.original == firstCopy.original)
                        repair.addDefinition(variable, copy.copiedDefinitions[index]);
                }
            }
        }
        repair.run();
    }

    Vector<Test> m_tests;
    IndexMap<BlockID, uint32_t> m_positionOfBlock;
    Vector<Copy, 4> m_copies;
};

} // anonymous namespace

PassOutcome threadJumps(FunctionPassContext& context)
{
    PassOutcome outcome = ParameterBranchThreader(context).run();
    outcome |= DecidedBranchThreader(context).run();
    if (hasTwoFunctionTestsForOneType(context))
        outcome |= RepeatedTestThreader(context).run();
    if (outcome == PassOutcome::Changed)
        context.keptAnalyses = allAnalyses;
    return outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
