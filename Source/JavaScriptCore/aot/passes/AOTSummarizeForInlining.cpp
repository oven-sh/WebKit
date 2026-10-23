/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSummarizeForInlining.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTCallGraph.h"
#include "AOTCodeSizeEstimate.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFreshResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTPropertyAccess.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <atomic>
#include <limits>
#include <optional>
#include <wtf/Atomics.h>
#include <wtf/CommaPrinter.h>
#include <wtf/GenerationalSet.h>
#include <wtf/PrintStream.h>
#include <wtf/SaturatingArithmetic.h>

namespace JSC { namespace AOT { namespace Typed {

ASCIILiteral nameOf(InlineBarrier barrier)
{
    switch (barrier) {
    case InlineBarrier::Unknown:
        return "Unknown"_s;
    case InlineBarrier::None:
        return "None"_s;
    case InlineBarrier::NoBody:
        return "NoBody"_s;
    case InlineBarrier::NotInlinable:
        return "NotInlinable"_s;
    case InlineBarrier::NeverInlined:
        return "NeverInlined"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool isBoundToFrame(const Function& function, const Block& block, uint32_t indexInBlock)
{
    Opcode opcode = function.at(block.values[indexInBlock]).opcode;
    if (boundToOf(opcode) != BoundTo::Frame)
        return false;
    bool readsCaughtValue = opcode == Opcode::CatchValue || opcode == Opcode::CatchException;
    return !readsCaughtValue || indexInBlock >= indexAfterCaughtValues(function, block);
}

bool isWrapperCreatedByEngine(const Function& function)
{
    switch (function.creator.kind()) {
    case CreatorRecord::Kind::ResultWrapper:
    case CreatorRecord::Kind::ParameterWrapper:
    case CreatorRecord::Kind::EntryChecks:
    case CreatorRecord::Kind::BoundFunction:
        return true;
    case CreatorRecord::Kind::Coroutine:
        return function.hasOrdinaryFrame() && function.role != FunctionRole::AsyncWorker;
    case CreatorRecord::Kind::None:
    case CreatorRecord::Kind::Entrypoint:
    case CreatorRecord::Kind::Version:
    case CreatorRecord::Kind::ResultWorker:
    case CreatorRecord::Kind::Serializer:
    case CreatorRecord::Kind::Parser:
    case CreatorRecord::Kind::Validator:
    case CreatorRecord::Kind::ChainStep:
    case CreatorRecord::Kind::StubBody:
    case CreatorRecord::Kind::TableLookup:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasOperationBoundToFrame(const Function& function, const Block& block)
{
    if (boundToOf(block.terminator.kind) == BoundTo::Frame)
        return true;
    for (uint32_t index = 0; index < block.values.size(); ++index) {
        if (isBoundToFrame(function, block, index))
            return true;
    }
    return false;
}

static bool canEndHeadBlock(TerminatorKind kind)
{
    switch (kind) {
    case TerminatorKind::Jump:
    case TerminatorKind::Branch:
    case TerminatorKind::Switch:
    case TerminatorKind::Return:
        return true;
    case TerminatorKind::SwitchString:
    case TerminatorKind::ReturnCall:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::TryCallInline:
    case TerminatorKind::TestAndBranch:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isRepeated(BuiltinCallbackTimes times)
{
    switch (times) {
    case BuiltinCallbackTimes::Repeatedly:
    case BuiltinCallbackTimes::PerElement:
        return true;
    case BuiltinCallbackTimes::Later:
    case BuiltinCallbackTimes::Once:
    case BuiltinCallbackTimes::PerEvent:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isStoreByKey(Opcode opcode)
{
    return storedOf(opcode) && receiverOf(opcode) && Value::opcodesWithKey.contains(opcode);
}

static bool isCoroutineOrCreatedByOne(const Function& function)
{
    return function.hasAny({ FunctionFlags::IsAsync, FunctionFlags::IsGenerator }) || function.creator.is<CreatorRecord::Coroutine>();
}

static bool entersCoroutine(const Program& program, const Function& function)
{
    if (isCoroutineOrCreatedByOne(function))
        return true;
    auto* entrypoint = function.creator.tryAs<CreatorRecord::Entrypoint>();
    return entrypoint && entrypoint->target && isCoroutineOrCreatedByOne(program.at(entrypoint->target));
}

static bool isDeadEnd(const Value& value, NOESCAPE const Invocable<bool(FunctionID)> auto& neverReturns)
{
    if (value.type == TypeTable::never)
        return true;
    return value.opcode == Opcode::CallDirect && neverReturns(value.function());
}

namespace {

enum class FeasibleSuccessors : uint8_t { All, HandlerOnly, None };

} // anonymous namespace

static FeasibleSuccessors feasibleSuccessorsOf(const Function& function, const Block& block, NOESCAPE const Invocable<bool(FunctionID)> auto& neverReturns)
{
    bool hasDeadEnd = std::ranges::any_of(block.values, [&](ValueID id) {
        return isDeadEnd(function.at(id), neverReturns);
    });
    if (!hasDeadEnd)
        return FeasibleSuccessors::All;
    return block.handler ? FeasibleSuccessors::HandlerOnly : FeasibleSuccessors::None;
}

static bool isFeasibleEdge(Edge edge, FeasibleSuccessors feasibleSuccessors)
{
    switch (feasibleSuccessors) {
    case FeasibleSuccessors::All:
        return true;
    case FeasibleSuccessors::HandlerOnly:
        return edge.isHandlerEdge();
    case FeasibleSuccessors::None:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static BitVector computeNoReturnBlocks(const Program& program, const Function& function, const CFG& cfg, NOESCAPE const Invocable<bool(FunctionID)> auto& neverReturns)
{
    if (entersCoroutine(program, function))
        return { };

    unsigned numberOfBlocks = function.blocks.size();
    Vector<FeasibleSuccessors> feasibleSuccessors(FillWith { }, numberOfBlocks, FeasibleSuccessors::All);
    BitVector mayReturn(numberOfBlocks);
    Vector<BlockID, 16> worklist;
    for (BlockID id : cfg.reversePostOrder()) {
        const Block& block = function.at(id);
        feasibleSuccessors[id.index()] = feasibleSuccessorsOf(function, block, neverReturns);
        if (feasibleSuccessors[id.index()] != FeasibleSuccessors::All)
            continue;
        if (returnsToCaller(block.terminator.kind)) {
            mayReturn.quickSet(id.index());
            worklist.append(id);
        }
    }
    while (!worklist.isEmpty()) {
        for (Edge edge : cfg.incomingEdges(worklist.takeLast())) {
            unsigned predecessor = edge.from.index();
            if (isFeasibleEdge(edge, feasibleSuccessors[predecessor]) && !mayReturn.quickSet(predecessor))
                worklist.append(edge.from);
        }
    }
    BitVector result;
    for (BlockID id : cfg.reversePostOrder()) {
        if (!mayReturn.quickGet(id.index()))
            result.ensureSizeAndSet(id.index(), numberOfBlocks);
    }
    return result;
}

namespace {

class FunctionSummarizer {
public:
    FunctionSummarizer(const AnalysisContext& context, const Function& function)
        : m_build(context.build)
        , m_program(context.program)
        , m_function(function)
        , m_analyses(context, function)
    {
        RELEASE_ASSERT(context.flow);
    }

    InliningSummary summarize(HeatClass heat)
    {
        InliningSummary summary;
        summary.barrier = computeBarrier();
        if (summary.barrier == InlineBarrier::NoBody)
            return summary;

        findBlockSets(summary);
        summary.noReturnBlocks = computeNoReturnBlocks(m_program, m_function, m_analyses.cfg(), [](FunctionID) {
            return false;
        });
        summary.neverReturns = summary.noReturnBlocks.get(entryBlock.index());
        if (summary.barrier != InlineBarrier::None)
            return summary;

        summary.codeBytes = computeBlockBytes();
        summary.isWrapper = isWrapperCreatedByEngine(m_function);
        if (isCold(heat))
            return summary;

        if (m_function.results.size() == 1) {
            findHead(summary);
            findConditions(summary);
        }
        findCalledParameters(summary);
        findIteratedParameters(summary);
        findReceiverParameters(summary);
        return summary;
    }

private:
    InlineBarrier computeBarrier() const
    {
        if (!m_function.hasBody())
            return InlineBarrier::NoBody;
        if (!m_function.mayBeInlined() || m_function.isModuleBody())
            return InlineBarrier::NotInlinable;
        RuntimeFunctionID runtimeFunction = m_function.enclosingRuntimeFunction;
        if (runtimeFunction && m_program.builtins().at(runtimeFunction).flags.contains(RuntimeFunctionFlag::IsNeverInlined))
            return InlineBarrier::NeverInlined;
        return InlineBarrier::None;
    }

    void findBlockSets(InliningSummary& summary)
    {
        for (BlockID id : m_analyses.cfg().reversePostOrder()) {
            const Block& block = m_function.at(id);
            if (block.handler && m_function.at(block.handler->block).role != BlockRole::IterationCleanup)
                summary.blocksWithHandlers.set(id.index());
            if (hasOperationBoundToFrame(m_function, block))
                summary.blocksBoundToFrame.set(id.index());
            if (block.terminator.kind == TerminatorKind::Return && !block.terminator.valueOperand())
                summary.hasBareReturn = true;
        }
    }

    bool isSlowPath(BlockID id) const
    {
        return m_function.at(id).frequency == BlockFrequency::Rare && m_function.at(entryBlock).frequency != BlockFrequency::Rare;
    }

    uint32_t computeBlockBytes()
    {
        m_blockBytes.fill(0, m_function.blocks.size());
        uint32_t codeBytes = 0;
        for (BlockID id : m_analyses.cfg().reversePostOrder()) {
            if (isSlowPath(id))
                continue;
            uint32_t bytes = estimatedCodeBytes(m_program, m_function, m_function.at(id));
            m_blockBytes[id.index()] = bytes;
            codeBytes = saturatingSum<uint32_t>(codeBytes, bytes);
        }
        return codeBytes;
    }

    std::optional<uint32_t> entryParameterBehind(ValueID value) const
    {
        size_t index = m_function.at(entryBlock).parameters.find(sameObjectThroughCasts(m_function, value));
        if (index == notFound)
            return std::nullopt;
        return static_cast<uint32_t>(index);
    }

    std::optional<uint32_t> entryParameterWithIdentityOf(ValueID value)
    {
        if (std::optional<uint32_t> index = entryParameterBehind(value))
            return index;
        return entryParameterBehind(m_analyses.identityOf(value));
    }

    bool isAdmissibleInHead(const InliningSummary& summary, BlockID id)
    {
        const Block& block = m_function.at(id);
        if (!canEndHeadBlock(block.terminator.kind) || block.handler || summary.blocksBoundToFrame.get(id.index()))
            return false;
        return std::ranges::all_of(block.values, [&](ValueID value) {
            const Effects& effects = m_analyses.effectsOf(value).normal;
            return !effects.mayRunOtherCode() && effects.writes.containsOnly(bookkeepingHeaps);
        });
    }

    void findHead(InliningSummary& summary)
    {
        BitVector headBlocks;
        uint32_t headBytes = 0;
        BitVector seen;
        Vector<BlockID, 8> queue { entryBlock };
        for (unsigned next = 0; next < queue.size(); ++next) {
            BlockID id = queue[next];
            if (seen.set(id.index()))
                continue;
            uint32_t bytes = m_blockBytes[id.index()];
            if (!isAdmissibleInHead(summary, id) || m_build.exceeds(LimitID::MaxInlineHeadBytes, uint64_t { headBytes } + bytes, m_analyses.subject()))
                continue;
            headBlocks.set(id.index());
            headBytes += bytes;
            auto& targets = m_function.at(id).terminator.targets;
            for (uint32_t i = 0; i < targets.size(); ++i) {
                if (m_analyses.blockFrequencies().classOf(Edge { id, i }) == BlockFrequency::Normal)
                    queue.append(targets[i].block);
            }
        }
        bool changed;
        do {
            changed = false;
            for (BlockID id : queue) {
                auto& terminator = m_function.at(id).terminator;
                if (!headBlocks.get(id.index()) || terminator.kind == TerminatorKind::Return)
                    continue;
                if (terminator.targets.containsIf([&](auto& target) { return headBlocks.get(target.block.index()); }))
                    continue;
                headBlocks.clear(id.index());
                headBytes -= m_blockBytes[id.index()];
                changed = true;
            }
        } while (changed);
        bool returns = false;
        for (size_t b : headBlocks)
            returns |= m_function.blocks[b].terminator.kind == TerminatorKind::Return;
        if (!returns || !headBlocks.get(entryBlock.index()))
            return;
        summary.headBlocks = WTF::move(headBlocks);
        summary.headBytes = headBytes;
    }

    std::optional<uint32_t> findDecidingParameter(ValueID condition)
    {
        if (!m_visitedValues.size())
            m_visitedValues.resize(m_function.values.size());
        m_visitedValues.clear();

        std::optional<uint32_t> decidingParameter;
        uint64_t chainLength = 0;
        Vector<ValueID, 16> worklist { condition };
        while (!worklist.isEmpty()) {
            ValueID id = worklist.takeLast();
            if (m_visitedValues.contains(id.index()))
                continue;
            m_visitedValues.add(id.index());
            const Value& value = m_function.at(id);
            if (value.opcode == Opcode::Parameter) {
                std::optional<uint32_t> index = entryParameterWithIdentityOf(id);
                if (!index || (decidingParameter && *decidingParameter != *index))
                    return std::nullopt;
                decidingParameter = index;
                continue;
            }
            if (isRematerializable(value))
                continue;
            if (!m_analyses.effectsOf(id).normal.isPure() || m_build.exceeds(LimitID::MaxConditionChainLength, ++chainLength, m_analyses.subject(condition)))
                return std::nullopt;
            value.forEachOperand([&](ValueID operand, uint32_t) {
                worklist.append(operand);
            });
        }
        return decidingParameter;
    }

    void findConditions(InliningSummary& summary)
    {
        CFG& cfg = m_analyses.cfg();
        Vector<BlockID, 2> branchingBlocks;
        Vector<BlockID, 1> switchingBlocks;
        for (BlockID block : cfg.reversePostOrder()) {
            auto& terminator = m_function.at(block).terminator;
            if (terminator.kind == TerminatorKind::Switch || terminator.kind == TerminatorKind::SwitchString) {
                if (std::optional<uint32_t> entryParameter = entryParameterWithIdentityOf(terminator.subjectOperand())) {
                    bool isOnStrings = terminator.kind == TerminatorKind::SwitchString;
                    summary.switches.append(ParameterSwitch { *entryParameter, isOnStrings, { }, 0 });
                    summary.switches.last().caseRanges.grow(terminator.targets.size() - 1);
                    for (size_t position = 0; position + 1 < terminator.targets.size(); ++position)
                        summary.switches.last().bytesOnlyInCase.append({ isOnStrings ? int64_t { terminator.strings()[position].index() } : terminator.cases()[position], 0 });
                    switchingBlocks.append(block);
                }
                continue;
            }
            if (terminator.kind != TerminatorKind::Branch)
                continue;
            if (std::optional<uint32_t> entryParameter = findDecidingParameter(terminator.conditionOperand())) {
                summary.conditions.append(ParameterCondition { terminator.conditionOperand(), *entryParameter, 0, 0 });
                branchingBlocks.append(block);
            }
        }
        if (branchingBlocks.isEmpty() && switchingBlocks.isEmpty())
            return;

        struct Subtree {
            uint32_t* ownBytes { nullptr };
            DominatedRange* range { nullptr };
            uint32_t totalBytes { 0 };
        };
        constexpr uint32_t noSubtree = std::numeric_limits<uint32_t>::max();
        Vector<Subtree, 8> subtrees;
        Vector<uint32_t> subtreeAtRoot(FillWith { }, m_function.blocks.size(), noSubtree);
        auto addSubtree = [&](const Target& target, uint32_t& bytes, DominatedRange& range) {
            if (!cfg.singleIncomingEdge(target.block))
                return;
            subtreeAtRoot[target.block.index()] = subtrees.size();
            subtrees.append(Subtree { &bytes, &range });
        };
        for (size_t i = 0; i < branchingBlocks.size(); ++i) {
            auto& terminator = m_function.at(branchingBlocks[i]).terminator;
            addSubtree(terminator.taken(), summary.conditions[i].bytesOnlyIfTrue, summary.conditions[i].rangeIfTrue);
            addSubtree(terminator.notTaken(), summary.conditions[i].bytesOnlyIfFalse, summary.conditions[i].rangeIfFalse);
        }
        for (size_t i = 0; i < switchingBlocks.size(); ++i) {
            auto& terminator = m_function.at(switchingBlocks[i]).terminator;
            ParameterSwitch& parameterSwitch = summary.switches[i];
            for (size_t position = 0; position < parameterSwitch.bytesOnlyInCase.size(); ++position)
                addSubtree(terminator.targets[position], parameterSwitch.bytesOnlyInCase[position].second, parameterSwitch.caseRanges[position]);
            addSubtree(terminator.otherwise(), parameterSwitch.bytesOnlyOtherwise, parameterSwitch.rangeOtherwise);
        }

        Vector<uint32_t, 16> enclosingSubtrees;
        uint32_t nextPreorderIndex = 0;
        auto enter = [&](BlockID block) {
            uint32_t preorderIndex = nextPreorderIndex++;
            if (uint32_t subtree = subtreeAtRoot[block.index()]; subtree != noSubtree) {
                subtrees[subtree].range->begin = preorderIndex;
                enclosingSubtrees.append(subtree);
            }
            if (enclosingSubtrees.isEmpty())
                return;
            Subtree& innermost = subtrees[enclosingSubtrees.last()];
            *innermost.ownBytes = saturatingSum<uint32_t>(*innermost.ownBytes, m_blockBytes[block.index()]);
            innermost.totalBytes = saturatingSum<uint32_t>(innermost.totalBytes, m_blockBytes[block.index()]);
        };
        auto leave = [&](BlockID block) {
            if (subtreeAtRoot[block.index()] == noSubtree)
                return;
            Subtree& subtree = subtrees[enclosingSubtrees.takeLast()];
            subtree.range->end = nextPreorderIndex;
            if (!subtree.totalBytes)
                *subtree.range = { };
            if (!enclosingSubtrees.isEmpty()) {
                Subtree& parent = subtrees[enclosingSubtrees.last()];
                parent.totalBytes = saturatingSum<uint32_t>(parent.totalBytes, subtree.totalBytes);
            }
        };
        m_analyses.dominators().walkTree(enter, leave);

        Vector<uint32_t, 4> totalBytesIfFalse(branchingBlocks.size(), [&](size_t i) {
            uint32_t subtree = subtreeAtRoot[m_function.at(branchingBlocks[i]).terminator.notTaken().block.index()];
            return subtree == noSubtree ? 0 : subtrees[subtree].totalBytes;
        });
        findBytesBehindFailedTests(summary, branchingBlocks.span(), totalBytesIfFalse.span());

        summary.conditions.removeAllMatching([](const ParameterCondition& condition) {
            return condition.rangeIfTrue.begin == condition.rangeIfTrue.end && condition.rangeIfFalse.begin == condition.rangeIfFalse.end && !condition.isTestInFrontOfSharedBytes;
        });
    }

    void findBytesBehindFailedTests(InliningSummary& summary, std::span<const BlockID> branchingBlocks, std::span<const uint32_t> totalBytesIfFalse)
    {
        CFG& cfg = m_analyses.cfg();
        BitVector failedTargets(m_function.blocks.size());
        Vector<size_t, 4> tests;
        for (size_t i = 0; i < branchingBlocks.size(); ++i) {
            BlockID failed = m_function.at(branchingBlocks[i]).terminator.notTaken().block;
            if (!cfg.singleIncomingEdge(failed) || m_function.at(failed).frequency <= m_function.at(entryBlock).frequency)
                continue;
            failedTargets.set(failed.index());
            tests.append(i);
        }
        if (tests.size() < 2)
            return;

        BitVector reachedWhereTestsPass = cfg.blocksReachableFrom(cfg.root(), failedTargets);
        uint32_t bytesBehindTests = 0;
        for (BlockID block : cfg.reversePostOrder()) {
            if (!reachedWhereTestsPass.get(block.index()))
                bytesBehindTests = saturatingSum<uint32_t>(bytesBehindTests, m_blockBytes[block.index()]);
        }
        uint32_t ownedBytes = 0;
        for (size_t i : tests) {
            bool isNested = std::ranges::any_of(tests, [&](size_t other) {
                return other != i && summary.conditions[other].rangeIfFalse.contains(summary.conditions[i].rangeIfFalse);
            });
            if (!isNested)
                ownedBytes = saturatingSum<uint32_t>(ownedBytes, totalBytesIfFalse[i]);
        }
        if (bytesBehindTests <= ownedBytes)
            return;

        summary.bytesOnlyIfSomeTestFails = bytesBehindTests - ownedBytes;
        for (size_t i : tests)
            summary.conditions[i].isTestInFrontOfSharedBytes = true;
    }

    bool isRepeatedBuiltinCallback(const Use& use) const
    {
        if (use.kind != Use::Kind::Operand)
            return false;
        const Value& user = m_function.at(use.user);
        if (user.opcode != Opcode::CallBuiltin)
            return false;
        BuiltinOperand operand = builtinOperandAt(user, use.operandIndex);
        return std::ranges::any_of(m_program.builtins().at(user.overload()).flow.callbacks, [&](const BuiltinCallback& callback) {
            return callback.callee == operand && isRepeated(callback.times);
        });
    }

    OperandUses declaredUsesAt(const Use& use) const
    {
        switch (use.kind) {
        case Use::Kind::Operand:
            return m_function.at(use.user).operandUses(use.operandIndex);
        case Use::Kind::TerminatorOperand:
            return m_function.at(use.block).terminator.operandUses(use.operandIndex);
        case Use::Kind::EdgeArgument:
        case Use::Kind::HandlerArgument:
        case Use::Kind::BlockSubject:
            return { };
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    // The unknown summary claims every calling use. Code that nobody sees is as unseen after inlining, so there only the operation's own declaration counts.
    bool isCalledAt(const ObjectUse& objectUse) const
    {
        if (!objectUse.uses.containsAny(callingUses))
            return false;
        return !objectUse.uses.containsAll(callingUses) || declaredUsesAt(objectUse.use).containsAny(callingUses);
    }

    void findCalledParameters(InliningSummary& summary)
    {
        auto& entryParameters = m_function.at(entryBlock).parameters;
        auto mayBeFunction = [&](ValueID parameter) {
            return m_program.typeOf(m_function, parameter).mayBeFunction();
        };
        if (!entryParameters.containsIf(mayBeFunction))
            return;

        const EscapeAnalysis& escape = m_analyses.escape();
        if (escape.gaveUp())
            return;
        const BlockCycles& cycles = m_analyses.blockCycles();
        auto noteCallsOf = [&](uint32_t i, ValueID definition) {
            std::optional<ObjectID> object = escape.objectDefinedBy(definition);
            if (!object)
                return;
            for (const ObjectUse& objectUse : escape.usesOf(*object)) {
                if (!isCalledAt(objectUse))
                    continue;
                summary.calledParameters.set(i);
                if (cycles.isInCycle(objectUse.use.block) || isRepeatedBuiltinCallback(objectUse.use)) {
                    summary.parametersCalledInCycles.set(i);
                    break;
                }
            }
        };
        for (uint32_t i = 0; i < entryParameters.size(); ++i) {
            if (mayBeFunction(entryParameters[i]))
                noteCallsOf(i, entryParameters[i]);
        }
    }

    static bool iterates(const Value& user, uint32_t operandIndex)
    {
        if (user.opcode == Opcode::StepGenerator)
            return operandIndex == OperandIndex::StepGenerator::generator;
        return user.opcode == Opcode::IteratorOpen && operandIndex == OperandIndex::IteratorOpen::iterable && user.iteratorKind() == IteratorKind::Sync;
    }

    void findIteratedParameters(InliningSummary& summary)
    {
        bool hasLoop = std::ranges::any_of(m_function.blocks, [&](const Block& block) {
            return std::ranges::any_of(block.values, [&](ValueID id) {
                Opcode opcode = m_function.at(id).opcode;
                return opcode == Opcode::IteratorOpen || opcode == Opcode::StepGenerator;
            });
        });
        if (!hasLoop)
            return;

        const EscapeAnalysis& escape = m_analyses.escape();
        if (escape.gaveUp())
            return;
        auto& entryParameters = m_function.at(entryBlock).parameters;
        for (uint32_t i = 0; i < entryParameters.size(); ++i) {
            std::optional<ObjectID> object = escape.objectDefinedBy(entryParameters[i]);
            if (!object)
                continue;
            bool isIterated = std::ranges::any_of(escape.usesOf(*object), [&](const ObjectUse& objectUse) {
                return objectUse.use.kind == Use::Kind::Operand && iterates(m_function.at(objectUse.use.user), objectUse.use.operandIndex);
            });
            if (isIterated)
                summary.iteratedParameters.set(i);
        }
    }

    void findReceiverParameters(InliningSummary& summary)
    {
        auto& entryParameters = m_function.at(entryBlock).parameters;
        Vector<ReceiverParameter, 8> receivers(entryParameters.size(), [](size_t index) {
            return ReceiverParameter { static_cast<uint32_t>(index), 0, 0 };
        });
        for (BlockID block : m_analyses.cfg().reversePostOrder()) {
            if (isSlowPath(block))
                continue;
            for (ValueID id : m_function.at(block).values) {
                const Value& access = m_function.at(id);
                bool isStore = isStoreByKey(access.opcode);
                if (!isNamedAccess(access.opcode) && !isStore)
                    continue;
                ValueID receiver = access.operand(*receiverOf(access.opcode));
                std::optional<uint32_t> index = entryParameterBehind(m_analyses.identityOf(receiver));
                if (!index)
                    continue;
                receivers[*index].stores += isStore;
                receivers[*index].accessesByName += isNamedAccess(access.opcode) && !m_program.typeOf(m_function, receiver).layouts;
            }
        }
        for (const ReceiverParameter& receiver : receivers) {
            if (receiver.accessesByName || receiver.stores)
                summary.receiverParameters.append(receiver);
        }
    }

    BuildContext& m_build;
    const Program& m_program;
    const Function& m_function;
    FunctionAnalyses m_analyses;
    Vector<uint32_t> m_blockBytes;
    GenerationalSet<uint32_t> m_visitedValues;
};

} // anonymous namespace

InliningSummary summarizeFunctionForInlining(const AnalysisContext& context, const Function& function, HeatClass heat)
{
    return FunctionSummarizer(context, function).summarize(heat);
}

static void propagateNeverReturns(const Program& program, const CallGraph& callGraph, ParallelArray<InliningSummary>& summaries, const ParallelLoops& loops)
{
    Vector<FunctionID> worklist = findIndicesInParallel(loops, summaries.size(), [&](unsigned index) {
        return summaries[index].neverReturns;
    }).map([](uint32_t index) {
        return FunctionID(index);
    });
    BitVector noReturnFunctions(summaries.size());
    for (FunctionID function : worklist)
        noReturnFunctions.quickSet(function.index());
    auto neverReturns = [&](FunctionID callee) {
        return noReturnFunctions.get(callee.index());
    };
    while (!worklist.isEmpty()) {
        Vector<FunctionID> callers;
        for (FunctionID callee : worklist) {
            for (FunctionID caller : callGraph.callersOf(callee)) {
                const InliningSummary& summary = summaries[caller.index()];
                if (summary.neverReturns || summary.barrier == InlineBarrier::Unknown || summary.barrier == InlineBarrier::NoBody)
                    continue;
                callers.append(caller);
            }
        }
        sortAndRemoveDuplicates(loops, callers);
        forEachIndexInParallel(loops, callers.size(), [&](unsigned index) {
            const Function& function = program.at(callers[index]);
            summaries[callers[index].index()].noReturnBlocks = computeNoReturnBlocks(program, function, CFG(function), neverReturns);
        });

        worklist.shrink(0);
        for (FunctionID caller : callers) {
            InliningSummary& summary = summaries[caller.index()];
            if (!summary.noReturnBlocks.get(entryBlock.index()))
                continue;
            summary.neverReturns = true;
            noReturnFunctions.quickSet(caller.index());
            worklist.append(caller);
        }
    }
}

InliningSummaries summarizeForInlining(const Program& program, const CallGraph& callGraph, const Heat& heat, const FlowResults& flow, BuildContext& build)
{
    const ParallelLoops& loops = build.parallelLoops();
    Vector<uint32_t> largestFirst = largeFunctions(program, loops);
    ParallelArray<InliningSummary> summaries(loops, program.functions.size(), largestFirst.span(), [&](unsigned index) {
        FunctionID id(index);
        if (!build.isEnabled(PassID::SummarizeForInlining, id))
            return InliningSummary { };
        AnalysisContext context { build, program, &flow, nullptr, id };
        return summarizeFunctionForInlining(context, program.at(id), heat.of(program, id));
    });
    propagateNeverReturns(program, callGraph, summaries, loops);

    InliningSummaries result;
    result.m_summaries = WTF::move(summaries);
    result.m_build = &build;
    result.m_flow = &flow;
    result.m_returnsFreshAggregate.fill(InliningSummaries::Answer::NotAsked, program.functions.size());
    result.m_locksOfAnswers = makeUniqueWithoutFastMallocCheck<std::array<Lock, InliningSummaries::numberOfLocks>>();
    return result;
}

bool InliningSummaries::returnsFreshAggregate(const Program& program, FunctionID id) const
{
    RELEASE_ASSERT(contains(id));
    if (m_summaries[id.index()].barrier != InlineBarrier::None)
        return false;

    AtomicRef answer(m_returnsFreshAggregate[id.index()]);
    Answer known = answer.load(std::memory_order_relaxed);
    if (known != Answer::NotAsked)
        return known == Answer::Yes;

    Locker locker { (*m_locksOfAnswers)[id.index() % numberOfLocks] };
    known = answer.load(std::memory_order_relaxed);
    if (known != Answer::NotAsked)
        return known == Answer::Yes;
    FunctionAnalyses analyses(AnalysisContext { *m_build, program, m_flow, nullptr, id }, program.at(id));
    known = Typed::returnsFreshAggregate(analyses) ? Answer::Yes : Answer::No;
    answer.store(known, std::memory_order_relaxed);
    return known == Answer::Yes;
}

static void dumpIndices(PrintStream& out, ASCIILiteral opening, ASCIILiteral prefix, const BitVector& indices)
{
    CommaPrinter separator(" "_s, opening);
    for (size_t index : indices)
        out.print(separator, prefix, index);
    if (separator.didPrint())
        out.print("]");
}

void InliningSummaries::dump(PrintStream& out, const Program& program) const
{
    for (unsigned index = 0; index < program.functions.size(); ++index) {
        FunctionID id(index);
        if (!program.at(id).hasBody())
            continue;
        const InliningSummary& summary = of(program, id);
        out.print("f", index, ": barrier=", nameOf(summary.barrier), " bytes=", summary.codeBytes);
        dumpIndices(out, " head=["_s, "b"_s, summary.headBlocks);
        if (!summary.headBlocks.isEmpty())
            out.print(" headBytes=", summary.headBytes);
        dumpIndices(out, " noReturn=["_s, "b"_s, summary.noReturnBlocks);
        if (summary.neverReturns)
            out.print(" neverReturns");
        dumpIndices(out, " boundToFrame=["_s, "b"_s, summary.blocksBoundToFrame);
        dumpIndices(out, " handlers=["_s, "b"_s, summary.blocksWithHandlers);
        if (summary.hasBareReturn)
            out.print(" bareReturn");
        if (summary.isWrapper)
            out.print(" wrapper");
        if (contains(id) && returnsFreshAggregate(program, id))
            out.print(" returnsFreshAggregate");
        dumpIndices(out, " called=["_s, ""_s, summary.calledParameters);
        dumpIndices(out, " calledInCycles=["_s, ""_s, summary.parametersCalledInCycles);
        dumpIndices(out, " iterated=["_s, ""_s, summary.iteratedParameters);
        for (const ParameterCondition& condition : summary.conditions) {
            out.print(" condition(v", condition.condition.index(), " p", condition.entryParameter, " true=", condition.bytesOnlyIfTrue, " false=", condition.bytesOnlyIfFalse, ")");
            if (condition.isTestInFrontOfSharedBytes)
                out.print(" guardsSharedBytes(v", condition.condition.index(), ")");
        }
        for (const ParameterSwitch& parameterSwitch : summary.switches) {
            out.print(" switch(p", parameterSwitch.entryParameter);
            for (auto [key, bytes] : parameterSwitch.bytesOnlyInCase) {
                if (parameterSwitch.isOnStrings)
                    out.print(" s", key, "=", bytes);
                else
                    out.print(" ", key, "=", bytes);
            }
            out.print(" otherwise=", parameterSwitch.bytesOnlyOtherwise, ")");
        }
        if (summary.bytesOnlyIfSomeTestFails)
            out.print(" someTestFails=", summary.bytesOnlyIfSomeTestFails);
        for (const ReceiverParameter& receiver : summary.receiverParameters)
            out.print(" receiver(p", receiver.entryParameter, " accesses=", receiver.accessesByName, " stores=", receiver.stores, ")");
        out.print("\n");
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
