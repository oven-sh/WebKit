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
#include <algorithm>
#include <optional>
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
    case CreatorRecord::Kind::BoundFunction:
        return true;
    case CreatorRecord::Kind::Coroutine:
        return function.hasOrdinaryFrame();
    case CreatorRecord::Kind::None:
    case CreatorRecord::Kind::Entrypoint:
    case CreatorRecord::Kind::Version:
    case CreatorRecord::Kind::ResultWorker:
    case CreatorRecord::Kind::Continuation:
    case CreatorRecord::Kind::Serializer:
    case CreatorRecord::Kind::Parser:
    case CreatorRecord::Kind::Validator:
    case CreatorRecord::Kind::ChainStep:
    case CreatorRecord::Kind::StubBody:
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

static bool isCoroutine(const Function& function)
{
    return function.hasAny({ FunctionFlags::IsAsync, FunctionFlags::IsGenerator }) || function.creator.is<CreatorRecord::Coroutine>();
}

static bool entersCoroutine(const Program& program, const Function& function)
{
    if (isCoroutine(function))
        return true;
    auto* entrypoint = function.creator.tryAs<CreatorRecord::Entrypoint>();
    return entrypoint && entrypoint->target && isCoroutine(program.at(entrypoint->target));
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
        summary.returnsFreshAggregate = returnsFreshAggregate(m_analyses);
        if (isCold(heat))
            return summary;

        if (m_function.results.size() == 1) {
            findHead(summary);
            findConditions(summary);
        }
        findCalledParameters(summary);
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

    uint32_t computeBlockBytes()
    {
        m_blockBytes.fill(0, m_function.blocks.size());
        uint32_t codeBytes = 0;
        for (BlockID id : m_analyses.cfg().reversePostOrder()) {
            uint32_t bytes = estimatedCodeBytes(m_program, m_function, m_function.at(id));
            m_blockBytes[id.index()] = bytes;
            codeBytes = saturatingSum<uint32_t>(codeBytes, bytes);
        }
        return codeBytes;
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

        auto& entryParameters = m_function.at(entryBlock).parameters;
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
                size_t index = entryParameters.find(id);
                if (index == notFound || (decidingParameter && *decidingParameter != index))
                    return std::nullopt;
                decidingParameter = static_cast<uint32_t>(index);
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
        for (BlockID block : cfg.reversePostOrder()) {
            auto& terminator = m_function.at(block).terminator;
            if (terminator.kind != TerminatorKind::Branch)
                continue;
            if (std::optional<uint32_t> entryParameter = findDecidingParameter(terminator.conditionOperand())) {
                summary.conditions.append(ParameterCondition { terminator.conditionOperand(), *entryParameter, 0, 0 });
                branchingBlocks.append(block);
            }
        }
        if (branchingBlocks.isEmpty())
            return;

        Vector<uint32_t*> subtreeOwners(FillWith { }, m_function.blocks.size(), nullptr);
        auto addOwner = [&](const Target& target, uint32_t& bytes) {
            if (cfg.singleIncomingEdge(target.block))
                subtreeOwners[target.block.index()] = &bytes;
        };
        for (size_t i = 0; i < branchingBlocks.size(); ++i) {
            auto& terminator = m_function.at(branchingBlocks[i]).terminator;
            addOwner(terminator.taken(), summary.conditions[i].bytesOnlyIfTrue);
            addOwner(terminator.notTaken(), summary.conditions[i].bytesOnlyIfFalse);
        }

        Vector<uint32_t*, 16> enclosingOwners;
        auto enter = [&](BlockID block) {
            if (uint32_t* owner = subtreeOwners[block.index()])
                enclosingOwners.append(owner);
            if (!enclosingOwners.isEmpty())
                *enclosingOwners.last() = saturatingSum<uint32_t>(*enclosingOwners.last(), m_blockBytes[block.index()]);
        };
        auto leave = [&](BlockID block) {
            if (subtreeOwners[block.index()])
                enclosingOwners.removeLast();
        };
        m_analyses.dominators().walkTree(enter, leave);

        summary.conditions.removeAllMatching([](const ParameterCondition& condition) {
            return !condition.bytesOnlyIfTrue && !condition.bytesOnlyIfFalse;
        });
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
        for (uint32_t i = 0; i < entryParameters.size(); ++i) {
            if (!mayBeFunction(entryParameters[i]))
                continue;
            std::optional<ObjectID> object = escape.objectDefinedBy(entryParameters[i]);
            if (!object)
                continue;
            for (const ObjectUse& objectUse : escape.usesOf(*object)) {
                if (!isCalledAt(objectUse))
                    continue;
                summary.calledParameters.set(i);
                if (cycles.isInCycle(objectUse.use.block) || isRepeatedBuiltinCallback(objectUse.use)) {
                    summary.parametersCalledInCycles.set(i);
                    break;
                }
            }
        }
    }

    void findReceiverParameters(InliningSummary& summary)
    {
        auto& entryParameters = m_function.at(entryBlock).parameters;
        Vector<uint32_t, 8> accessesByName(FillWith { }, entryParameters.size(), 0u);
        for (BlockID block : m_analyses.cfg().reversePostOrder()) {
            for (ValueID id : m_function.at(block).values) {
                const Value& access = m_function.at(id);
                if (!isNamedAccess(access.opcode))
                    continue;
                ValueID receiver = access.operand(*receiverOf(access.opcode));
                if (m_program.typeOf(m_function, receiver).layouts)
                    continue;
                size_t index = entryParameters.find(sameObjectThroughCasts(m_function, receiver));
                if (index != notFound)
                    ++accessesByName[index];
            }
        }
        for (uint32_t i = 0; i < entryParameters.size(); ++i) {
            if (accessesByName[i])
                summary.receiverParameters.append(ReceiverParameter { i, accessesByName[i] });
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

static void propagateNeverReturns(const Program& program, const CallGraph& callGraph, PerFunction<InliningSummary>& summaries, const ParallelLoops& loops)
{
    BitVector noReturnFunctions(summaries.size());
    Vector<FunctionID> worklist;
    for (unsigned index = 0; index < summaries.size(); ++index) {
        if (!summaries[FunctionID(index)].neverReturns)
            continue;
        noReturnFunctions.quickSet(index);
        worklist.append(FunctionID(index));
    }
    auto neverReturns = [&](FunctionID callee) {
        return noReturnFunctions.get(callee.index());
    };
    while (!worklist.isEmpty()) {
        Vector<FunctionID> callers;
        for (FunctionID callee : worklist) {
            for (FunctionID caller : callGraph.callersOf(callee)) {
                const InliningSummary& summary = summaries[caller];
                if (summary.neverReturns || summary.barrier == InlineBarrier::Unknown || summary.barrier == InlineBarrier::NoBody)
                    continue;
                callers.append(caller);
            }
        }
        sortAndRemoveDuplicates(loops, callers);
        forEachIndexInParallel(loops, callers.size(), [&](unsigned index) {
            const Function& function = program.at(callers[index]);
            summaries[callers[index]].noReturnBlocks = computeNoReturnBlocks(program, function, CFG(function), neverReturns);
        });

        worklist.shrink(0);
        for (FunctionID caller : callers) {
            InliningSummary& summary = summaries[caller];
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
    unsigned count = program.functions.size();
    PerFunction<InliningSummary> summaries(count);
    forEachIndexInParallel(build.parallelLoops(), count, [&](unsigned index) {
        FunctionID id(index);
        if (!build.isEnabled(PassID::SummarizeForInlining, id))
            return;
        AnalysisContext context { build, program, &flow, nullptr, id };
        summaries[id] = summarizeFunctionForInlining(context, program.at(id), heat.of(program, id));
    });
    propagateNeverReturns(program, callGraph, summaries, build.parallelLoops());

    InliningSummaries result;
    result.m_summaries = WTF::move(summaries);
    return result;
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
        if (summary.returnsFreshAggregate)
            out.print(" returnsFreshAggregate");
        dumpIndices(out, " called=["_s, ""_s, summary.calledParameters);
        dumpIndices(out, " calledInCycles=["_s, ""_s, summary.parametersCalledInCycles);
        for (const ParameterCondition& condition : summary.conditions)
            out.print(" condition(v", condition.condition.index(), " p", condition.entryParameter, " true=", condition.bytesOnlyIfTrue, " false=", condition.bytesOnlyIfFalse, ")");
        for (const ReceiverParameter& receiver : summary.receiverParameters)
            out.print(" receiver(p", receiver.entryParameter, " accesses=", receiver.accessesByName, ")");
        out.print("\n");
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
