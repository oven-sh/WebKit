/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSplitFunctionsAtCalls.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTChooseCallingConventions.h"
#include "AOTFlowResults.h"
#include "AOTFrameAllocationPointers.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionEditing.h"
#include "AOTOperatorForms.h"
#include "AOTPass.h"
#include "AOTRegionCloner.h"
#include "AOTThreads.h"
#include <algorithm>
#include <optional>
#include <span>
#include <utility>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct SplitInputs {
    AnalysisContext contextOf(FunctionID id) const { return { build, program, &flow, &effects, id }; }

    BuildContext& build;
    const Program& program;
    const FlowResults& flow;
    const EffectSummaries& effects;
    const CallingConventions& conventions;
};

} // anonymous namespace

static bool canBePassedByTailCall(OpaqueKind kind)
{
    return !isFunctionLocal(kind) && !allowsDirectCallsOnly(kind);
}

static bool canBeInSignature(const Program& program, TypeID id)
{
    const Type& type = program.at(id);
    return !type.environment && (!type.opaque || canBePassedByTailCall(*type.opaque));
}

static bool canBeReturnedByTailCall(const Program& program, TypeID id)
{
    const Type& type = program.at(id);
    return type.isJavaScriptValue() && !type.isNever();
}

static bool canBeSplit(const SplitInputs& inputs, FunctionID id)
{
    const Function& function = inputs.program.at(id);
    return function.hasBody()
        && function.hasOrdinaryFrame()
        && function.mayBeCloned()
        && function.mayChangeSignature()
        && function.results.size() == 1
        && !inputs.flow.runsAtMostOnce(inputs.program, id)
        && inputs.build.isEnabled(PassID::SplitFunctionsAtCalls, id);
}

static bool isCall(Opcode opcode)
{
    switch (effectRuleOf(opcode)) {
    case EffectRule::Call:
    case EffectRule::Builtin:
        return true;
    case EffectRule::Declared:
    case EffectRule::Operator:
    case EffectRule::Access:
    case EffectRule::Inline:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool needsFrame(const EffectsOnEdges& effects)
{
    const Effects& normal = effects.normal;
    return effects.mayThrow() || normal.mayCollect || normal.mayCallUnknownCode || normal.maySuspend || normal.mayBeTerminated;
}

// A shape epoch renews a guard of the function that read it: as a parameter of another function it is a number that proves nothing.
static BitVector findShapeEpochs(const Function& function)
{
    BitVector epochs(function.values.size());
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            if (function.at(id).opcode == Opcode::GetShapeEpoch)
                epochs.quickSet(id.index());
        }
    }

    for (bool changed = !epochs.isEmpty(); changed;) {
        changed = false;
        for (const Block& block : function.blocks) {
            for (const Target& target : block.terminator.targets) {
                const auto& parameters = function.at(target.block).parameters;
                for (size_t index = 0; index < target.arguments.size(); ++index) {
                    if (epochs.quickGet(target.arguments[index].index()))
                        changed |= !epochs.quickSet(parameters[index].index());
                }
            }
        }
    }
    return epochs;
}

static ValueID tailCallOf(const Block& block)
{
    return block.terminator.kind == TerminatorKind::ReturnCall ? block.terminator.callOperand() : ValueID();
}

namespace {

struct ContinuationPlan {
    BlockID head;
    BlockFrequency frequency { BlockFrequency::Normal };
    Vector<BlockID> region;
    Vector<BlockID, 2> jumpTargets;
    Vector<ValueID> liveIns;
    Function signature;
};

struct SplitPlan {
    unsigned indexOf(BlockID head) const
    {
        size_t index = continuations.findIf([&](const ContinuationPlan& continuation) {
            return continuation.head == head;
        });
        RELEASE_ASSERT(index != notFound);
        return static_cast<unsigned>(index);
    }

    Vector<ContinuationPlan> continuations;
    BitVector entryBlocks;
};

class FunctionSplitter {
public:
    FunctionSplitter(const SplitInputs& inputs, FunctionID id, const Function& function)
        : m_program(inputs.program)
        , m_id(id)
        , m_function(function)
        , m_conventions(inputs.conventions)
        , m_analyses(inputs.contextOf(id), m_function)
        , m_numberOfBlocks(m_function.blocks.size())
        , m_needsFrame(m_numberOfBlocks)
    {
    }

    bool findEntryPart()
    {
        if (!canBeReturnedByTailCall(m_program, m_function.result().type) || blockNeedsFrame(entryBlock))
            return false;

        const CFG& cfg = m_analyses.cfg();
        m_isReachable = cfg.blocksReachableFrom(cfg.root(), { });
        for (unsigned block : m_isReachable) {
            if (blockNeedsFrame(BlockID(block)))
                m_needsFrame.set(block);
        }
        if (m_needsFrame.isEmpty())
            return false;
        includeCycles(m_isReachable);

        m_entryBlocks = cfg.blocksReachableFrom(cfg.root(), m_needsFrame);
        if (!hasExit(m_entryBlocks))
            return false;
        findHeads(m_entryBlocks);
        m_framePointers.emplace(m_function);
        m_shapeEpochs = findShapeEpochs(m_function);
        return true;
    }

    // A head that starts with uses of values that cannot leave the function is cut after the last of them, so they stay in the entry part.
    Vector<ProgramPoint> findCutsInsideHeads()
    {
        Vector<ProgramPoint> cuts;
        for (const Head& head : m_heads) {
            if (uint32_t index = findCutInside(head.block))
                cuts.append(ProgramPoint { head.block, index });
        }
        return cuts;
    }

    std::optional<SplitPlan> planContinuations()
    {
        const CFG& cfg = m_analyses.cfg();
        SplitPlan plan;
        plan.entryBlocks = m_entryBlocks;
        bool hasTooManyHeads = build().exceeds(LimitID::MaxContinuationsPerFunction, m_heads.size(), m_analyses.subject());
        if (!hasTooManyHeads) {
            if (m_framePointers->isEmpty())
                findJumpTargets();
            findRegions();
        }
        if (hasTooManyHeads || exceedsSizeLimit(m_isReachable)) {
            if (!cutAtCommonDominator())
                return std::nullopt;
            plan.entryBlocks = cfg.blocksReachableFrom(cfg.root(), only(m_heads[0].block.index()));
            if (!hasExit(plan.entryBlocks))
                return std::nullopt;
        }

        Vector<unsigned, 16> order;
        for (unsigned i = 0; i < m_heads.size(); ++i)
            order.append(i);
        std::ranges::sort(order, [&](unsigned a, unsigned b) {
            return std::pair { m_heads[a].reached.bitCount(), a } < std::pair { m_heads[b].reached.bitCount(), b };
        });
        plan.continuations.grow(m_heads.size());
        for (unsigned index : order) {
            if (!planContinuation(plan, index))
                return std::nullopt;
        }
        return plan;
    }

private:
    struct Head {
        BlockID block;
        BitVector reached;
        BitVector jumpTargets;
        BitVector region;
    };

    BuildContext& build() const { return m_analyses.context().build; }

    bool operationNeedsFrame(ValueID id)
    {
        const Value& value = m_function.at(id);
        if (boundToOf(value.opcode) == BoundTo::Frame || isCall(value.opcode))
            return true;
        if (hasOperatorForm(value.opcode))
            return mayCallRuntime(value.opcode, value.form());
        return needsFrame(m_analyses.effectsOf(id));
    }

    bool terminatorNeedsFrame(BlockID id)
    {
        return boundToOf(m_function.at(id).terminator.kind) == BoundTo::Frame || needsFrame(m_analyses.effectsOfTerminator(id));
    }

    bool blockNeedsFrame(BlockID id)
    {
        const Block& block = m_function.at(id);
        if (block.handler || terminatorNeedsFrame(id))
            return true;
        ValueID tailCall = tailCallOf(block);
        return block.values.containsIf([&](ValueID value) {
            return value != tailCall && operationNeedsFrame(value);
        });
    }

    // The state that tells whether a value is specified is known in one function: in a part that is cut off the value would be a parameter like any other.
    bool canBeParameter(ValueID id)
    {
        return !m_framePointers->contains(id) && !m_shapeEpochs.get(id.index()) && canBeInSignature(m_program, m_function.at(id).type) && !mayBeUnspecified(m_analyses, id);
    }

    bool isDefinedBefore(ValueID id, ProgramPoint point)
    {
        ValueLocation location = m_analyses.useLists().locationOf(id);
        return location.block != point.block || location.placement != ValuePlacement::InBlock || location.index < point.index;
    }

    uint32_t findCutInside(BlockID id)
    {
        const Block& block = m_function.at(id);
        if (block.handler || m_analyses.blockCycles().isInCycle(id))
            return 0;

        ValueID tailCall = tailCallOf(block);
        ProgramPoint cut = ProgramPoint::atHeadOf(id);
        for (uint32_t index = 0; index < block.values.size(); ++index) {
            ValueID value = block.values[index];
            if (value == tailCall || operationNeedsFrame(value))
                break;
            bool staysInEntryPart = false;
            m_function.at(value).forEachChild([&](ValueID operand) {
                staysInEntryPart |= !canBeParameter(operand) && isDefinedBefore(operand, cut);
            });
            if (staysInEntryPart)
                cut.index = index + 1;
        }
        return cut.index;
    }

    bool hasExit(const BitVector& blocks) const
    {
        for (unsigned index : blocks) {
            if (returnsToCaller(m_function.blocks[index].terminator.kind))
                return true;
        }
        return false;
    }

    size_t sizeOf(const BitVector& blocks) const
    {
        size_t size = 0;
        for (unsigned block : blocks)
            size += m_function.blocks[block].values.size() + 1;
        return size;
    }

    BitVector only(unsigned block) const
    {
        BitVector result(m_numberOfBlocks);
        result.set(block);
        return result;
    }

    void includeCycles(const BitVector& isReachable)
    {
        const BlockCycles& cycles = m_analyses.blockCycles();
        BitVector cycleNeedsFrame(cycles.forest().size());
        for (unsigned block : m_needsFrame) {
            if (cycles.isInCycle(BlockID(block)))
                cycleNeedsFrame.set(*cycles.outermostCycleOf(BlockID(block)));
        }
        for (unsigned block : isReachable) {
            if (cycles.isInCycle(BlockID(block)) && cycleNeedsFrame.get(*cycles.outermostCycleOf(BlockID(block))))
                m_needsFrame.set(block);
        }
    }

    bool isInCycleWith(BlockID block, const BitVector& others)
    {
        const BlockCycles& cycles = m_analyses.blockCycles();
        if (!cycles.isInCycle(block))
            return false;
        for (unsigned other : others) {
            if (cycles.areInSameCycle(block, BlockID(other)))
                return true;
        }
        return false;
    }

    void findHeads(const BitVector& entryBlocks)
    {
        const CFG& cfg = m_analyses.cfg();
        BitVector isHead(m_numberOfBlocks);
        for (unsigned block : entryBlocks) {
            for (BlockID target : cfg.successors(BlockID(block))) {
                if (m_needsFrame.get(target.index()))
                    isHead.set(target.index());
            }
        }
        for (unsigned block : isHead)
            m_heads.append(Head { BlockID(block), { }, { }, { } });
    }

    void findJumpTargets()
    {
        const CFG& cfg = m_analyses.cfg();
        const BlockFrequencies& frequencies = m_analyses.blockFrequencies();
        for (Head& head : m_heads)
            head.reached = cfg.blocksReachableFrom(head.block, { });
        for (Head& head : m_heads) {
            for (const Head& other : m_heads) {
                if (head.block != other.block && frequencies.classOf(other.block) != BlockFrequency::Normal && head.reached.get(other.block.index()) && !other.reached.get(head.block.index()))
                    head.jumpTargets.set(other.block.index());
            }
        }
    }

    void findRegions()
    {
        const CFG& cfg = m_analyses.cfg();
        for (Head& head : m_heads)
            head.region = cfg.blocksReachableFrom(head.block, head.jumpTargets);
    }

    size_t copiedSize() const
    {
        size_t size = 0;
        for (const Head& head : m_heads)
            size += sizeOf(head.region);
        return size;
    }

    bool exceedsSizeLimit(const BitVector& isReachable) const
    {
        if (copiedSize() * 100 <= sizeOf(isReachable) * build().limit(LimitID::MaxContinuationSizePercent))
            return false;
        build().gaveUp(LimitID::MaxContinuationSizePercent, m_analyses.subject());
        return true;
    }

    bool cutAtCommonDominator()
    {
        const CFG& cfg = m_analyses.cfg();
        const Dominators& dominators = m_analyses.dominators();
        BlockID head;
        for (unsigned block : m_needsFrame)
            head = head ? dominators.nearestCommonDominator(head, BlockID(block)) : BlockID(block);
        while (head != cfg.root() && isInCycleWith(head, cfg.blocksReachableFrom(cfg.root(), only(head.index()))))
            head = dominators.idom(head);
        if (head == cfg.root())
            return false;
        m_heads.clear();
        m_heads.append(Head { head, { }, { }, cfg.blocksReachableFrom(head, { }) });
        return true;
    }

    bool planContinuation(SplitPlan& plan, unsigned index)
    {
        const Head& head = m_heads[index];
        ContinuationPlan& continuation = plan.continuations[index];
        continuation.head = head.block;
        continuation.frequency = m_analyses.blockFrequencies().classOf(head.block);
        for (unsigned block : head.region)
            continuation.region.append(BlockID(block));
        if (RegionCloner::checkCanExtract(m_analyses, continuation.region.span()) != CloneCheck::CanClone)
            return false;
        continuation.jumpTargets = findUsedJumpTargets(head, continuation.region.span());
        continuation.liveIns = findLiveIns(plan, head, continuation);
        continuation.signature = createSignature(index);
        return appendParameters(continuation.signature, continuation.liveIns.span())
            && appendParameters(continuation.signature, m_function.at(head.block).parameters.span())
            && fitsInRegisters(continuation.signature);
    }

    Vector<BlockID, 2> findUsedJumpTargets(const Head& head, std::span<const BlockID> region)
    {
        Vector<BlockID, 2> jumpTargets;
        if (head.jumpTargets.isEmpty())
            return jumpTargets;
        const CFG& cfg = m_analyses.cfg();
        BitVector isJumpTarget(m_numberOfBlocks);
        for (BlockID block : region) {
            for (BlockID target : cfg.successors(block)) {
                if (head.jumpTargets.get(target.index()))
                    isJumpTarget.set(target.index());
            }
        }
        for (unsigned block : isJumpTarget)
            jumpTargets.append(BlockID(block));
        return jumpTargets;
    }

    Vector<ValueID> findLiveIns(const SplitPlan& plan, const Head& head, const ContinuationPlan& continuation)
    {
        Vector<ValueID> liveIns = RegionCloner::findLiveIns(m_function, continuation.region.span());
        if (continuation.jumpTargets.isEmpty())
            return liveIns;
        const UseLists& useLists = m_analyses.useLists();
        BitVector isLiveIn(m_function.values.size());
        for (ValueID liveIn : liveIns)
            isLiveIn.set(liveIn.index());
        for (BlockID target : continuation.jumpTargets) {
            for (ValueID liveIn : plan.continuations[plan.indexOf(target)].liveIns) {
                if (!head.region.get(useLists.blockOf(liveIn).index()))
                    isLiveIn.set(liveIn.index());
            }
        }
        liveIns.shrink(0);
        for (unsigned value : isLiveIn)
            liveIns.append(ValueID(value));
        return liveIns;
    }

    Function createSignature(unsigned index) const
    {
        Function signature;
        signature.name = m_function.name;
        signature.position = m_function.position;
        signature.text = m_function.text;
        signature.role = FunctionRole::Continuation;
        signature.flags = m_function.flags & OptionSet<FunctionFlags> { FunctionFlags::IsSloppy, FunctionFlags::IsChecked };
        if (m_function.isHiddenInStackTraces())
            signature.flags.add(FunctionFlags::IsHiddenInStackTraces);
        signature.enclosingRuntimeFunction = m_function.enclosingRuntimeFunction;
        signature.thisType = DeclaredType { .type = TypeTable::anything };
        signature.results = m_function.results;
        signature.creator = CreatorRecord::Continuation { m_id, index };
        signature.inlineFrames = m_function.inlineFrames;
        signature.capabilities = initialCapabilitiesOf(signature);
        return signature;
    }

    // A part does with the value of a parameter no more than the whole function did, so what flow says of the parameter holds for the part.
    ParameterOrigin originOf(ValueID id)
    {
        size_t parameter = m_function.at(entryBlock).parameters.find(m_analyses.identities().identityOf(id));
        if (parameter == notFound)
            return { };
        const Derivation& derivation = m_function.derivation;
        if (!derivation.source)
            return { static_cast<uint32_t>(parameter) };
        return parameter < derivation.parameterMap.size() ? derivation.parameterMap[parameter] : ParameterOrigin { };
    }

    bool appendParameters(Function& signature, std::span<const ValueID> values)
    {
        for (ValueID id : values) {
            if (!canBeParameter(id))
                return false;
            const Value& value = m_function.at(id);
            signature.parameters.append(DeclaredType { .type = value.type, .written = value.written(), .predicted = value.prediction() });
            signature.derivation.parameterMap.append(originOf(id));
        }
        return true;
    }

    bool fitsInRegisters(const Function& signature) const
    {
        return !computeArgumentLocations(m_program, signature, FunctionConvention { }, m_conventions.registers()).numberOfStackArguments;
    }

    const Program& m_program;
    FunctionID m_id;
    const Function& m_function;
    const CallingConventions& m_conventions;
    FunctionAnalyses m_analyses;
    unsigned m_numberOfBlocks;
    BitVector m_isReachable;
    BitVector m_needsFrame;
    BitVector m_entryBlocks;
    Vector<Head> m_heads;
    std::optional<FramePointers> m_framePointers;
    BitVector m_shapeEpochs;
};

struct PlannedSplit {
    const Function& body(const Program& program LIFETIME_BOUND) const LIFETIME_BOUND { return bodyWithCutHeads ? *bodyWithCutHeads : program.at(origin); }

    FunctionID origin;
    std::optional<Function> bodyWithCutHeads;
    SplitPlan plan;
    Vector<FunctionID> continuations;
};

struct SplitParts {
    Function entryPart;
    Vector<Function> continuations;
};

class PartBuilder {
public:
    PartBuilder(const SplitInputs& inputs, TypeArena& types, const PlannedSplit& split)
        : m_inputs(inputs)
        , m_types(types)
        , m_split(split)
        , m_origin(split.body(inputs.program))
    {
    }

    SplitParts run()
    {
        SplitParts parts;
        parts.continuations.reserveInitialCapacity(m_split.continuations.size());
        for (unsigned index = 0; index < m_split.continuations.size(); ++index)
            parts.continuations.append(createContinuation(index));
        parts.entryPart = createEntryPart();
        return parts;
    }

private:
    Function createEntryPart()
    {
        Function result = m_origin;
        IRBuilder builder(m_inputs.contextOf(m_split.origin), result, m_types);
        builder.setOrigin({ m_origin.position, 0 });
        Vector<BlockID> leavingBlocks(FillWith { }, m_origin.blocks.size(), BlockID());
        for (unsigned index = 0; index < m_split.continuations.size(); ++index) {
            const ContinuationPlan& continuation = m_split.plan.continuations[index];
            BlockID leavingBlock = addLeavingBlock(builder, index);
            emitTailCall(builder, leavingBlock, index, Arguments(continuation.liveIns));
            leavingBlocks[continuation.head.index()] = leavingBlock;
        }
        for (unsigned block : m_split.plan.entryBlocks) {
            auto& targets = result.blocks[block].terminator.targets;
            for (uint32_t i = 0; i < targets.size(); ++i) {
                BlockID leavingBlock = leavingBlocks[targets[i].block.index()];
                if (!leavingBlock)
                    continue;
                Target redirected = WTF::move(targets[i]);
                redirected.block = leavingBlock;
                builder.redirectEdge(Edge { BlockID(block), i }, WTF::move(redirected));
            }
        }
        compactFunction(result);
        return result;
    }

    Function createContinuation(unsigned index)
    {
        const ContinuationPlan& plan = m_split.plan.continuations[index];
        FunctionID id = m_split.continuations[index];
        Function result = m_inputs.program.at(id);
        IRBuilder builder(m_inputs.contextOf(id), result, m_types);
        builder.setOrigin({ m_origin.position, 0 });
        RegionCloner cloner(builder, m_origin);

        BlockID entry = addBlockCutFrom(builder, plan.head, BlockFrequency::Normal);
        for (ValueID liveIn : plan.liveIns)
            cloner.mapValue(liveIn, addParameterLike(builder, entry, liveIn));
        Arguments headArguments;
        for (ValueID parameter : m_origin.at(plan.head).parameters)
            headArguments.append(addParameterLike(builder, entry, parameter));
        Vector<BlockID, 2> leavingBlocks;
        for (BlockID target : plan.jumpTargets) {
            leavingBlocks.append(addLeavingBlock(builder, m_split.plan.indexOf(target)));
            cloner.mapBlock(target, leavingBlocks.last());
        }

        cloner.cloneBlocks(plan.region.span(), [](ValueID, Value&) { });
        builder.moveToEndOf(entry);
        builder.emitJump(cloner.cloneOf(plan.head), WTF::move(headArguments));

        for (unsigned i = 0; i < leavingBlocks.size(); ++i) {
            unsigned target = m_split.plan.indexOf(plan.jumpTargets[i]);
            Arguments liveIns;
            for (ValueID liveIn : m_split.plan.continuations[target].liveIns) {
                ValueID clone = cloner.cloneOf(liveIn);
                RELEASE_ASSERT(clone);
                liveIns.append(clone);
            }
            emitTailCall(builder, leavingBlocks[i], target, WTF::move(liveIns));
        }
        return result;
    }

    BlockID addBlockCutFrom(IRBuilder& builder, BlockID originBlock, BlockFrequency frequency)
    {
        BlockID block = builder.addBlock(frequency);
        builder.function().at(block).shape = m_origin.at(originBlock).shape;
        return block;
    }

    ValueID addParameterLike(IRBuilder& builder, BlockID block, ValueID originValue)
    {
        const Value& value = m_origin.at(originValue);
        ValueID parameter = builder.addParameter(block, value.type);
        Value& added = builder.function().at(parameter);
        added.setWritten(value.written());
        added.setPrediction(value.prediction());
        return parameter;
    }

    BlockID addLeavingBlock(IRBuilder& builder, unsigned target)
    {
        const ContinuationPlan& continuation = m_split.plan.continuations[target];
        BlockID leavingBlock = addBlockCutFrom(builder, continuation.head, continuation.frequency);
        for (ValueID parameter : m_origin.at(continuation.head).parameters)
            addParameterLike(builder, leavingBlock, parameter);
        return leavingBlock;
    }

    void emitTailCall(IRBuilder& builder, BlockID leavingBlock, unsigned target, Arguments&& liveIns)
    {
        liveIns.append(builder.function().at(leavingBlock).parameters.span());
        Value call = IRBuilder::make(Opcode::CallDirect);
        call.setFunction(m_split.continuations[target]);
        call.setArgumentsOperands(liveIns.span());
        builder.moveToEndOf(leavingBlock);
        ValueID id = builder.append(WTF::move(call));

        Terminator terminator = IRBuilder::makeTerminator(TerminatorKind::ReturnCall, { id });
        terminator.setRequirement(TailCallRequirement::Required);
        builder.emitTerminator(WTF::move(terminator));
    }

    const SplitInputs& m_inputs;
    TypeArena& m_types;
    const PlannedSplit& m_split;
    const Function& m_origin;
};

} // anonymous namespace

static void cutBlocks(const SplitInputs& inputs, TypeInterningPhase& scratchTypes, FunctionID id, Function& function, std::span<const ProgramPoint> cuts)
{
    TypeArena types(scratchTypes, TypeInterningPhase::Scratch);
    IRBuilder builder(inputs.contextOf(id), function, types);
    builder.setOrigin({ function.position, 0 });
    for (ProgramPoint cut : cuts)
        builder.splitBlock(cut);
}

static std::optional<PlannedSplit> planSplit(const SplitInputs& inputs, TypeInterningPhase& scratchTypes, FunctionID id)
{
    FunctionSplitter splitter(inputs, id, inputs.program.at(id));
    if (!splitter.findEntryPart())
        return std::nullopt;

    PlannedSplit split;
    split.origin = id;
    Vector<ProgramPoint> cuts = splitter.findCutsInsideHeads();
    std::optional<SplitPlan> plan;
    if (cuts.isEmpty())
        plan = splitter.planContinuations();
    else {
        split.bodyWithCutHeads = inputs.program.at(id);
        cutBlocks(inputs, scratchTypes, id, *split.bodyWithCutHeads, cuts.span());
        FunctionSplitter splitterOfCutBody(inputs, id, *split.bodyWithCutHeads);
        if (splitterOfCutBody.findEntryPart())
            plan = splitterOfCutBody.planContinuations();
    }
    if (!plan)
        return std::nullopt;
    split.plan = WTF::move(*plan);
    return split;
}

void splitFunctionsAtCalls(ProgramEditor& editor, const OptimizationInputs& inputs, BuildContext& build)
{
    RELEASE_ASSERT(inputs.callingConventions);
    const Program& program = editor.program();
    SplitInputs splitInputs { build, program, inputs.flow, inputs.effects, *inputs.callingConventions };
    unsigned numberOfFunctions = program.functions.size();
    TypeInterningPhase scratchTypes(program.types, 0u, TypeInterningPhase::Scratch);
    auto splits = collectInParallel<PlannedSplit>(build.parallelLoops(), numberOfFunctions, [&](unsigned index, Vector<PlannedSplit>& found) {
        FunctionID id(index);
        if (!canBeSplit(splitInputs, id))
            return;
        if (auto split = planSplit(splitInputs, scratchTypes, id))
            found.append(WTF::move(*split));
    });
    scratchTypes.discard();
    if (splits.isEmpty())
        return;

    Vector<FunctionID> origins;
    Vector<FunctionID> continuations;
    origins.reserveInitialCapacity(splits.size());
    for (PlannedSplit& split : splits) {
        for (ContinuationPlan& continuation : split.plan.continuations)
            split.continuations.append(editor.addFunctionLike(split.origin, WTF::move(continuation.signature)));
        origins.append(split.origin);
        continuations.appendVector(split.continuations);
    }

    editFunctionsInParallel(editor, build, origins.span(), continuations.span(), [&](unsigned index, FunctionID, Function& origin, TaskArenas& arenas) {
        const PlannedSplit& split = splits[index];
        SplitParts parts = PartBuilder(splitInputs, arenas.types, split).run();
        origin = WTF::move(parts.entryPart);
        for (unsigned i = 0; i < split.continuations.size(); ++i)
            editor.at(split.continuations[i]) = WTF::move(parts.continuations[i]);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
