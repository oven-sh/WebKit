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

// A prediction proves nothing, so an edge that skips it passes what it was made of.
static ValueID valueBehindPredictions(const Function& function, const Block& block, ValueID value)
{
    while (function.at(value).opcode == Opcode::Predict && block.values.contains(value))
        value = function.at(value).valueOperand();
    return value;
}

static Arguments argumentsWithoutTestAndPredictions(const Function& function, Edge edge)
{
    Arguments passed = argumentsWithoutTest(function, edge);
    for (ValueID& argument : passed)
        argument = valueBehindPredictions(function, function.at(edge.from), argument);
    return passed;
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
        Target target = substituteParameters(mergePoint, targetOf(m_function, chosenEdge).block, argumentsWithoutTestAndPredictions(m_function, chosenEdge), incomingArguments);
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

        Terminator dispatch = std::exchange(m_function.at(copied.copy).terminator, Terminator { });
        Target& chosen = dispatch.targets[targetIndex];
        m_builder.setOrigin(dispatch.origin());
        m_builder.moveToEndOf(copied.copy);
        m_builder.emitJump(chosen.block, WTF::move(chosen.arguments));
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

        m_function.at(source).terminator = Terminator { };
        m_builder.setOrigin(origin);
        m_builder.moveToEndOf(source);
        m_builder.emitBranch(condition, WTF::move(taken), WTF::move(notTaken));
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
        for (BlockID candidate : m_candidates)
            threadEdgesInto(candidate);
        return m_outcome;
    }

private:
    void findCandidates()
    {
        const CFG& cfg = m_analyses.cfg();
        for (BlockID block : cfg.reversePostOrder()) {
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
            return m_analyses.effectsOf(value).onAnyEdge().isRemovableWhenUnused();
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
            known.append({ id, value.isKnown() ? value : evaluateTypeTestOfArgument(mergePoint, m_function.at(id), incomingArguments) });
        }
        return lookUp(testedValueOf(mergePoint.terminator));
    }

    // The type of what an edge passes may decide a test that the type of the parameter leaves open.
    KnownValue evaluateTypeTestOfArgument(const Block& mergePoint, const Value& test, const Arguments& incomingArguments) const
    {
        if (test.opcode != Opcode::TypeTest)
            return { };
        ValueID tested = valueBehindPredictions(m_function, mergePoint, test.valueOperand());
        if (!mergePoint.parameters.contains(tested))
            return { };
        const Type& passed = m_program.typeOf(m_function, substituteParameter(mergePoint, tested, incomingArguments));
        switch (evaluateTypeTest(m_program, passed, m_context.types.at(test.testedType()))) {
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
                    addUserOutsideBlock(candidate, use.block);
            };
            const Block& mergePoint = m_function.at(candidate);
            for (ValueID parameter : mergePoint.parameters)
                addUsersOf(parameter);
            for (ValueID value : mergePoint.values)
                addUsersOf(value);
        }
    }

    void addUserOutsideBlock(BlockID candidate, BlockID user)
    {
        if (user != candidate)
            m_usersOutsideBlock[candidate].add(user);
    }

    bool reachesUser(BlockID candidate, BlockID start)
    {
        const IndexSet<BlockID>& users = m_usersOutsideBlock[candidate];
        if (users.isEmpty())
            return false;
        IndexSet<BlockID> seen;
        seen.add(candidate);
        seen.add(start);
        Vector<BlockID, 16> worklist { start };
        uint64_t remainingSteps = m_context.limit(LimitID::MaxThreadingReachabilitySteps);
        while (!worklist.isEmpty()) {
            BlockID block = worklist.takeLast();
            if (users.contains(block))
                return true;
            if (!remainingSteps--) {
                m_context.gaveUp(LimitID::MaxThreadingReachabilitySteps, testedValueOf(m_function.at(candidate).terminator));
                return true;
            }
            m_function.at(block).forEachTarget([&](const Target& target) {
                if (seen.add(target.block))
                    worklist.append(target.block);
            });
        }
        return false;
    }

    bool canJumpDirectlyAlong(Edge chosenEdge)
    {
        if (!canFollowWithoutTest(m_function, chosenEdge))
            return false;
        if (passesValuesOf(m_function.at(chosenEdge.from), argumentsWithoutTestAndPredictions(m_function, chosenEdge)))
            return false;
        return !reachesUser(chosenEdge.from, targetOf(m_function, chosenEdge).block);
    }

    void threadEdgesInto(BlockID candidate)
    {
        const Block& mergePoint = m_function.at(candidate);
        const Terminator& terminator = mergePoint.terminator;
        if (hasEdgeTo(terminator, candidate))
            return;
        Vector<std::optional<bool>, 2> canJumpDirectlyTo(FillWith { }, terminator.targets.size(), std::nullopt);
        for (Edge edge : m_incomingEdges[candidate]) {
            const Arguments& incomingArguments = targetOf(m_function, edge).arguments;
            std::optional<uint32_t> targetIndex = decidedTargetOf(terminator, evaluateBranch(mergePoint, incomingArguments));
            if (!targetIndex)
                continue;
            Edge chosenEdge { candidate, *targetIndex };
            std::optional<bool>& canJumpDirectly = canJumpDirectlyTo[*targetIndex];
            if (!canJumpDirectly)
                canJumpDirectly = canJumpDirectlyAlong(chosenEdge);
            if (!*canJumpDirectly)
                continue;
            std::optional<Target> target = createThreadedTarget(mergePoint, chosenEdge, incomingArguments);
            if (!target)
                continue;
            for (ValueID argument : target->arguments) {
                if (BlockID definingCandidate = m_definingCandidate[argument])
                    addUserOutsideBlock(definingCandidate, edge.from);
            }
            redirect(edge, WTF::move(*target));
        }
    }

    IndexMap<ValueID, BlockID> m_definingCandidate;
    IndexMap<BlockID, IndexSet<BlockID>> m_usersOutsideBlock;
};

} // anonymous namespace

PassOutcome threadJumps(FunctionPassContext& context)
{
    PassOutcome outcome = ParameterBranchThreader(context).run();
    outcome |= DecidedBranchThreader(context).run();
    return outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
