/*
 * Copyright (C) 2015-2021 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "AirGenerate.h"

#if ENABLE(B3_JIT)

#include "AirAllocateRegistersAndStackAndGenerateCode.h"
#include "AirAllocateRegistersByGreedy.h"
#include "AirAllocateStackByGraphColoring.h"
#include "AirCode.h"
#include "AirDuplicateReturns.h"
#include "AirEliminateDeadCode.h"
#include "AirFixObviousSpills.h"
#include "AirFixPartialRegisterStalls.h"
#include "AirFoldTrapsIntoAccesses.h"
#include "AirForwardRegisterCopies.h"
#include "AirFrameStates.h"
#include "AirGenerationContext.h"
#include "AirHoistCommonMoves.h"
#include "AirInstObserver.h"
#include "AirLogRegisterPressure.h"
#include "AirLowerAfterRegAlloc.h"
#include "AirLowerEntrySwitch.h"
#include "AirLowerStackArgs.h"
#include "AirMergeIdenticalBlocks.h"
#include "AirOpcodeUtils.h"
#include "AirOptimizeBlockOrder.h"
#include "AirOptimizePairedLoadStore.h"
#include "AirPlanFrame.h"
#include "AirReportUsedRegisters.h"
#include "AirResequenceMovesBeforePatches.h"
#include "AirReuseConditionFlags.h"
#include "AirShrinkWrapFrame.h"
#include "AirSplitLiveRangesAroundFrame.h"
#include "AirSimplifyCFG.h"
#include "AirStackColoring.h"
#include "AirValidate.h"
#include "B3Common.h"
#include "B3Procedure.h"
#include "CCallHelpers.h"
#include "CompilerTimingScope.h"
#include "DisallowMacroScratchRegisterUsage.h"
#include <span>
#include <wtf/IndexMap.h>

namespace JSC { namespace B3 { namespace Air {

namespace {

struct PipelineState {
    bool cfgMayHaveChanged { false };
    StackColoring stackColoring;
    FramePlacement framePlacement;
};

struct Step {
    ASCIILiteral name;
    bool (*isEnabled)(Code&, const PipelineState&);
    void (*run)(Code&, PipelineState&);
};

void begin(Code& code)
{
    // If we're doing super verbose dumping, the phase scope of any phase will already do a dump.
    if (shouldDumpIR(code.proc(), AirMode) && !shouldDumpIRAtEachPhase(AirMode)) {
        dataLog(tierName, "Initial air:\n");
        dataLog(code);
    }
    
    // We don't expect the incoming code to have predecessors computed.
    code.resetReachability();

    if (Options::dumpIonGraph()) [[unlikely]]
        code.appendIonGraphPass("InitialCFG"_s);

    if (shouldValidateIR(code.proc()))
        validate(code);
}

void prepareAtOptLevel0(Code& code)
{
    RELEASE_ASSERT(!code.policy().frame.emitsFrame);

    // FIXME: The name of this phase doesn't make much sense in O0 since we do this before
    // register allocation.
    lowerAfterRegAlloc(code);

    // Actually create entrypoints.
    lowerEntrySwitch(code);
    
    // This sorts the basic blocks in Code to achieve an ordering that maximizes the likelihood that a high
    // frequency successor is also the fall-through target.
    optimizeBlockOrder(code);

    if (shouldValidateIR(code.proc()))
        validate(code);

    if (shouldDumpIR(code.proc(), AirMode)) {
        dataLog("Air after ", code.lastPhaseName(), ", before generation:\n");
        dataLog(code);
    }

    code.m_generateAndAllocateRegisters = makeUniqueWithoutFastMallocCheck<GenerateAndAllocateRegisters>(code);
    code.m_generateAndAllocateRegisters->prepareForGeneration();
}

constexpr bool always(Code&, const PipelineState&)
{
    return true;
}

template<auto pass>
void runPass(Code& code, PipelineState&)
{
    pass(code);
}

constexpr Step simplifyCFGIfChanged {
    "simplifyCFG"_s,
    [](Code&, const PipelineState& state) { return state.cfgMayHaveChanged; },
    [](Code& code, PipelineState& state) {
        simplifyCFG(code);
        state.cfgMayHaveChanged = false;
    }
};

static constexpr Step steps[] = {
    { "simplifyCFG"_s, always, runPass<simplifyCFG> },

    // This is where we run our optimizations and transformations.
    // FIXME: Add Air optimizations.
    // https://bugs.webkit.org/show_bug.cgi?id=150456

    { "eliminateDeadCode"_s, always, runPass<eliminateDeadCode> },

    {
        "foldTrapsIntoAccesses"_s,
        [](Code& code, const PipelineState&) { return !!code.policy().peepholes.trapReach; },
        runPass<foldTrapsIntoAccesses>
    },

    {
        "moveBlocksWithoutSuccessorsToEnd"_s,
        [](Code& code, const PipelineState&) { return code.policy().layout.movesBlocksWithoutSuccessorsToEnd; },
        runPass<moveBlocksWithoutSuccessorsToEnd>
    },

    {
        "splitLiveRangesAroundFrame"_s,
        [](Code& code, const PipelineState&) { return code.policy().frame.emitsFrame && code.policy().splitting.splitsAroundFrame; },
        runPass<splitLiveRangesAroundFrame>
    },

    // Register allocation for all the Tmps that do not have a corresponding machine
    // register. After this phase, every Tmp has a reg.
    { "allocateRegistersByGreedy"_s, always, runPass<allocateRegistersByGreedy> },

    {
        "logRegisterPressure"_s,
        [](Code&, const PipelineState&) { return Options::logAirRegisterPressure(); },
        [](Code& code, PipelineState&) {
            dataLog("Register pressure after register allocation:\n");
            logRegisterPressure(code);
        }
    },

    { "lowerAfterRegAlloc"_s, always, runPass<lowerAfterRegAlloc> },

    // This replaces uses of spill slots with registers or constants if possible. It
    // does this by minimizing the amount that we perturb the already-chosen register
    // allocation. It may extend the live ranges of registers though.
    { "fixObviousSpills"_s, always, runPass<fixObviousSpills> },

    // This does first-fit allocation of stack slots using an interference graph plus a
    // bunch of other optimizations.
    {
        "allocateStackByGraphColoring"_s,
        always,
        [](Code& code, PipelineState& state) { state.stackColoring = allocateStackByGraphColoring(code); }
    },

    {
        "shrinkWrapFrame"_s,
        [](Code& code, const PipelineState&) { return code.policy().frame.emitsFrame; },
        [](Code& code, PipelineState& state) {
            state.framePlacement = shrinkWrapFrame(code, state.stackColoring);
            state.cfgMayHaveChanged |= state.framePlacement.didAddBlocks;
        }
    },
    {
        "planFrame"_s,
        [](Code& code, const PipelineState&) { return code.policy().frame.emitsFrame; },
        [](Code& code, PipelineState& state) {
            planFrame(code, state.stackColoring, state.framePlacement);
            state.stackColoring = { };
            state.framePlacement = { };
        }
    },

    // This turns all Stack and CallArg Args into Addr args that use the frame pointer.
    { "lowerStackArgs"_s, always, runPass<lowerStackArgs> },

    // If we coalesced moves then we can unbreak critical edges. This is the main reason for this
    // phase.
    { "simplifyCFG"_s, always, runPass<simplifyCFG> },

    {
        "hoistCommonMoves"_s,
        [](Code& code, const PipelineState&) { return code.policy().peepholes.hoistsCommonMoves; },
        [](Code& code, PipelineState& state) { state.cfgMayHaveChanged = hoistCommonMoves(code); }
    },
    simplifyCFGIfChanged,

    // Only reportUsedRegisters() removes the moves that this makes dead.
    {
        "forwardRegisterCopies"_s,
        [](Code& code, const PipelineState&) { return code.policy().peepholes.forwardsRegisterCopies && code.needsUsedRegisters(); },
        runPass<forwardRegisterCopies>
    },

    // Not worth a whole-graph liveness scan just for the dead assignments it also kills, so this runs
    // only for the used-register set that B3::StackmapValue reports to a patchpoint's generator.
    {
        "reportUsedRegisters"_s,
        [](Code& code, const PipelineState&) { return code.needsUsedRegisters(); },
        [](Code& code, PipelineState& state) { state.cfgMayHaveChanged |= reportUsedRegisters(code); }
    },

    // Attempt to remove false dependencies between instructions created by partial register changes.
    // This must be executed as late as possible as it depends on the instructions order and register
    // use, and it must follow reportUsedRegisters() when that runs, since that kills variable
    // assignments that seem dead. Luckily, this phase does not change register liveness, so that's OK.
    { "fixPartialRegisterStalls"_s, always, runPass<fixPartialRegisterStalls> },

    // Actually create entrypoints.
    {
        "lowerEntrySwitch"_s,
        always,
        [](Code& code, PipelineState& state) { state.cfgMayHaveChanged |= lowerEntrySwitch(code); }
    },
    simplifyCFGIfChanged,

    // We do this optimization at the very end of Air generation pipeline since it can be beneficial after
    // spills are lowered to load/store with the frame pointer or the stack pointer. And this is block-local
    // optimization, so this is more effective after simplifyCFG.
#if CPU(ARM64)
    {
        "optimizePairedLoadStore"_s,
        [](Code& code, const PipelineState&) { return code.policy().peepholes.pairsSpillAccesses; },
        runPass<optimizePairedLoadStore>
    },
#endif

    // This sorts the basic blocks in Code to achieve an ordering that maximizes the likelihood that a high
    // frequency successor is also the fall-through target.
    { "optimizeBlockOrder"_s, always, runPass<optimizeBlockOrder> },

    // These three read the block order through findNextBlock().
    {
        "resequenceMovesBeforePatches"_s,
        [](Code& code, const PipelineState&) { return code.policy().peepholes.resequencesMovesBeforePatches; },
        runPass<resequenceMovesBeforePatches>
    },
    {
        "mergeIdenticalBlocks"_s,
        [](Code& code, const PipelineState&) { return code.policy().layout.mergesIdenticalBlocks; },
        runPass<mergeIdenticalBlocks>
    },
    {
        "duplicateReturns"_s,
        [](Code& code, const PipelineState&) { return code.policy().layout.duplicatesReturns; },
        runPass<duplicateReturns>
    },
    {
        "routeRareBranchesThroughJumps"_s,
        [](Code& code, const PipelineState&) { return isX86() && code.policy().layout.placesRareBlocksInGaps; },
        runPass<routeRareBranchesThroughJumps>
    },

    // This has to be the last step that changes instructions: nothing may be inserted between a
    // definition of the flags and its uses.
    {
        "reuseConditionFlags"_s,
        [](Code& code, const PipelineState&) { return code.policy().peepholes.reusesConditionFlags; },
        runPass<reuseConditionFlags>
    },
};

void finish(Code& code)
{
    if (shouldValidateIR(code.proc()))
        validate(code);

    // Do a final dump of Air. Note that we have to do this even if we are doing per-phase dumping,
    // since the final generation is not a phase.
    if (shouldDumpIR(code.proc(), AirMode)) {
        dataLog("Air after ", code.lastPhaseName(), ", before generation:\n");
        dataLog(code);
    }
}

} // anonymous namespace

void prepareForGeneration(Code& code)
{
    Code* codes[] = { &code };
    prepareForGeneration(std::span<Code* const>(codes));
}

void prepareForGeneration(std::span<Code* const> codes)
{
    CompilerTimingScope timingScope("Total Air"_s, "prepareForGeneration"_s);

    Vector<PipelineState, 16> states(codes.size());

    for (Code* code : codes) {
        begin(*code);
        if (!code->optLevel())
            prepareAtOptLevel0(*code);
    }

    for (const Step& step : steps) {
        for (size_t index = 0; index < codes.size(); ++index) {
            Code& code = *codes[index];
            if (!code.optLevel() || code.proc().report().failure || !step.isEnabled(code, states[index]))
                continue;
            step.run(code, states[index]);
        }
    }

    for (Code* code : codes) {
        if (code->optLevel() && !code->proc().report().failure)
            finish(*code);
    }
}

static void generateWithAlreadyAllocatedRegisters(Code& code, CCallHelpers& jit)
{
    CompilerTimingScope timingScope("Air"_s, "generateWithAlreadyAllocatedRegisters"_s);

    DisallowMacroScratchRegisterUsage disallowScratch(jit);

    // And now, we generate code.
    GenerationContext context;
    context.code = &code;
    const bool emitsFrame = code.policy().frame.emitsFrame;
    std::optional<FrameStates> frameStates;
    if (emitsFrame) {
        frameStates.emplace(code);
        RELEASE_ASSERT(!frameStates->violation());
        context.frameIsSetUp = false;
    }
    context.blockLabels.resize(code.size());
    IndexMap<BasicBlock*, CCallHelpers::Label> labels(code.size());
    for (BasicBlock* block : code) {
        if (block->last().kind.opcode != Patch)
            continue;
        for (BasicBlock* successor : block->successorBlocks()) {
            if (!context.blockLabels[successor])
                context.blockLabels[successor] = Box<CCallHelpers::Label>::create();
        }
    }
    IndexMap<BasicBlock*, CCallHelpers::JumpList> blockJumps(code.size());

    auto link = [&] (CCallHelpers::Jump jump, BasicBlock* target) {
        if (labels[target].isSet()) {
            jump.linkTo(labels[target], &jit);
            return;
        }

        blockJumps[target].append(jump);
    };

    PCToOriginMap& pcToOriginMap = code.proc().pcToOriginMap();
    bool shouldPreserveB3Origins = code.shouldPreserveB3Origins();
    auto addItem = [&] (Inst& inst) {
        if (!shouldPreserveB3Origins)
            return;
        if (inst.origin)
            pcToOriginMap.appendItem(jit.labelIgnoringWatchpoints(), inst.origin->origin());
        else
            pcToOriginMap.appendItem(jit.labelIgnoringWatchpoints(), Origin());
    };

    Disassembler* disassembler = code.disassembler();

    InstObserver* observer = code.instObserver();
    const bool recordsRanges = disassembler || observer;
    auto didGenerate = [&](Inst& inst, CCallHelpers::Label start) {
        if (!recordsRanges)
            return;
        CCallHelpers::Label end = jit.labelIgnoringWatchpoints();
        if (disassembler)
            disassembler->addInst(&inst, start, end);
        if (observer)
            observer->didGenerate(inst, start, end);
    };

    unsigned numberOfLatePathsNoted = 0;
    auto noteNewLatePaths = [&] {
        for (; numberOfLatePathsNoted < context.latePaths.size(); ++numberOfLatePathsNoted) {
            if (!context.frameIsSetUp)
                context.latePathsWithoutFrame.set(numberOfLatePathsNoted);
        }
    };

    for (BasicBlock* block : code) {
        context.currentBlock = block;
        context.indexInBlock = UINT_MAX;
        blockJumps[block].link(&jit);
        CCallHelpers::Label label = jit.label();
        labels[block] = label;
        if (auto& sharedLabel = context.blockLabels[block])
            *sharedLabel = label;
        if (context.blockLabels[block])
            jit.indirectJumpTarget(); // Bun: a patchpoint has its address, for a jump table for one.

        if (emitsFrame)
            context.setFrameIsSetUp(jit, frameStates->atHead(block) == FrameState::SetUp);
        if (observer)
            observer->willGenerate(*block);

        if (disassembler)
            disassembler->startBlock(block, jit); 

        if (std::optional<unsigned> entrypointIndex = code.entrypointIndex(block)) {
            ASSERT(code.isEntrypoint(block));

            if (disassembler)
                disassembler->startEntrypoint(jit); 

            // Bun: the first begins with what signs the return address. Where a throw or an OSR entry goes on does not.
            if (*entrypointIndex)
                jit.indirectCallTarget();
            code.prologueGeneratorForEntrypoint(*entrypointIndex)->run(jit, code, context);
            noteNewLatePaths();

            if (disassembler)
                disassembler->endEntrypoint(jit); 
        } else
            ASSERT(!code.isEntrypoint(block));
        
        ASSERT(block->size() >= 1);
        for (unsigned i = 0; i < block->size() - 1; ++i) {
            context.indexInBlock = i;
            Inst& inst = block->at(i);
            addItem(inst);
            auto start = recordsRanges ? jit.labelIgnoringWatchpoints() : CCallHelpers::Label();
            CCallHelpers::Jump jump = inst.generate(jit, context);
            ASSERT_UNUSED(jump, !jump.isSet());
            noteNewLatePaths();
            didGenerate(inst, start);
        }

        context.indexInBlock = block->size() - 1;
        
        if (block->last().kind.opcode == Jump
            && block->successorBlock(0) == code.findNextBlock(block))
            continue;

        if (isReturn(block->last().kind.opcode)) {
            // We currently don't represent the full prologue/epilogue in Air, so we need to
            // have this override.
            addItem(block->last());
            auto start = recordsRanges ? jit.labelIgnoringWatchpoints() : CCallHelpers::Label();
            if (emitsFrame)
                jit.ret();
            else
                code.emitEpilogue(jit);
            didGenerate(block->last(), start);
            continue;
        }

        addItem(block->last());
        auto start = recordsRanges ? jit.labelIgnoringWatchpoints() : CCallHelpers::Label();
        CCallHelpers::Jump jump = block->last().generate(jit, context);
        noteNewLatePaths();
        didGenerate(block->last(), start);

        // The jump won't be set for patchpoints. It won't be set for Oops because then it won't have
        // any successors.
        if (jump.isSet()) {
            switch (block->numSuccessors()) {
            case 1:
                link(jump, block->successorBlock(0));
                break;
            case 2:
                link(jump, block->successorBlock(0));
                if (block->successorBlock(1) != code.findNextBlock(block))
                    link(jit.jump(), block->successorBlock(1));
                break;
            default:
                RELEASE_ASSERT_NOT_REACHED();
                break;
            }
        }
    }
    
    context.currentBlock = nullptr;
    context.indexInBlock = UINT_MAX;
    
    if (code.policy().layout.recordsRareCodeStart) [[unlikely]] {
        CCallHelpers::Label rareCodeStart = jit.labelIgnoringWatchpoints();
        for (unsigned index = code.size(); index-- > code.numberOfBlocksInChains();) {
            if (BasicBlock* block = code.at(index))
                rareCodeStart = labels[block];
        }
        code.recordRareCodeStart(rareCodeStart);
    }
    Vector<CCallHelpers::Label> entrypointLabels(code.numEntrypoints());
    for (unsigned i = code.numEntrypoints(); i--;)
        entrypointLabels[i] = labels[code.entrypoint(i).block()];
    code.setEntrypointLabels(WTF::move(entrypointLabels));

    pcToOriginMap.appendItem(jit.label(), Origin());
    // FIXME: Make late paths have Origins: https://bugs.webkit.org/show_bug.cgi?id=153689
    if (disassembler)
        disassembler->startLatePath(jit);

    for (unsigned index = 0; index < context.latePaths.size(); ++index) {
        if (code.shouldPreserveB3Origins())
            pcToOriginMap.appendItem(jit.labelIgnoringWatchpoints(), std::get<0>(context.latePaths[index]));
        Ref<GenerationContext::LatePath> latePath = std::get<1>(context.latePaths[index]);
        if (emitsFrame)
            context.setFrameIsSetUp(jit, !context.latePathsWithoutFrame.get(index));
        latePath->run(jit, context);
        noteNewLatePaths();
    }

    if (emitsFrame) {
        context.setFrameIsSetUp(jit, false);
        code.setFrameRanges(WTF::move(context.frameRanges));
    }

    if (disassembler)
        disassembler->endLatePath(jit);
    pcToOriginMap.appendItem(jit.labelIgnoringWatchpoints(), Origin());
}

void generate(Code& code, CCallHelpers& jit)
{
    if (code.optLevel())
        generateWithAlreadyAllocatedRegisters(code, jit);
    else
        code.m_generateAndAllocateRegisters->generate(jit);
}

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
