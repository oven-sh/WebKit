/*
 * Copyright (C) 2015-2020 Apple Inc. All rights reserved.
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
#include "B3Generate.h"

#if ENABLE(B3_JIT)

#include "AirGenerate.h"
#include "B3CanonicalizePrePostIncrements.h"
#include "B3CheckKnownProperties.h"
#include "B3CheckVariablesAreSet.h"
#include "B3Common.h"
#include "B3ConvertBranchesToSelects.h"
#include "B3DuplicateTails.h"
#include "B3EliminateCommonSubexpressions.h"
#include "B3EliminateDeadCode.h"
#include "B3EliminateRedundantPhiCycles.h"
#include "B3EliminateWasmGCAllocations.h"
#include "B3FixSSA.h"
#include "B3FoldComparisonsWithBounds.h"
#include "B3FoldPathConstants.h"
#include "B3HoistLoopInvariantValues.h"
#include "B3InferSwitches.h"
#include "B3LegalizeMemoryOffsets.h"
#include "B3LowerMacros.h"
#include "B3LowerMacrosAfterOptimizations.h"
#include "B3LowerRelocatableValues.h"
#include "B3LowerToAir.h"
#include "B3MaterializePinnedConstants.h"
#include "B3MergeAdjacentConstantStores.h"
#include "B3MoveConstants.h"
#include "B3NaturalLoops.h"
#include "B3OptimizeAssociativeExpressionTrees.h"
#include "B3PhaseScope.h"
#include "B3Policy.h"
#include "B3Procedure.h"
#include "B3ReduceDoubleToFloat.h"
#include "B3ReduceStrength.h"
#include "B3RotateLoops.h"
#include "B3SimplifyTerminals.h"
#include "B3SinkValues.h"
#include "B3SpecializeSelect.h"
#include "B3SplitBranchConditions.h"
#include "B3ThreadJumps.h"
#include "B3Validate.h"
#include "B3VersionLoops.h"
#include "CompilerTimingScope.h"
#include <wtf/OptionSet.h>

namespace JSC { namespace B3 {

namespace {

enum class Cleanup : uint8_t {
    FixSSA = 1 << 0,
    FoldPathConstants = 1 << 1,
    SimplifyTerminals = 1 << 2,
    EliminateDeadCode = 1 << 3,
};

using Cleanups = OptionSet<Cleanup>;

struct PipelineState {
    Cleanups pendingCleanups;
    Vector<BasicBlock*> topTestedVersionHeaders;
    bool hasLoops { false };
    bool didRotateLoops { false };
    bool didVersionLoops { false };
};

enum class NeededEstimate : uint8_t {
    None,
    EdgeClasses,
    BlockFrequencies,
};

struct Step {
    ASCIILiteral name;
    bool (*isEnabled)(Procedure&, const PipelineState&);
    Cleanups (*run)(Procedure&, PipelineState&);
    NeededEstimate neededEstimate { NeededEstimate::None };
};

bool always(Procedure&, const PipelineState&)
{
    return true;
}

bool hoistsInvariantValues(const Procedure& procedure)
{
    return procedure.policy().upstream.hoistsLoopInvariantValues;
}

Cleanups cleanupsIfChanged(bool changed, Cleanups cleanups)
{
    return changed ? cleanups : Cleanups { };
}

template<auto pass, auto... arguments>
Cleanups runPass(Procedure& procedure, PipelineState&)
{
    pass(procedure, arguments...);
    return { };
}

template<auto pass>
Cleanups runLoopPass(Procedure& procedure, PipelineState&)
{
    if (procedure.hasCycle() && procedure.naturalLoops().numLoops())
        pass(procedure);
    return { };
}

Cleanups runValidation(Procedure& procedure, PipelineState&)
{
    validate(procedure);
    return { };
}

Cleanups runNothing(Procedure&, PipelineState&)
{
    return { };
}

bool hasClientPhase(const Procedure& procedure, ClientPhase::Point point)
{
    return procedure.policy().clientPhases.containsIf([&](const ClientPhase& phase) {
        return phase.point == point;
    });
}

template<ClientPhase::Point point>
Cleanups runClientPhases(Procedure& procedure, PipelineState&)
{
    for (const ClientPhase& phase : procedure.policy().clientPhases) {
        if (phase.point != point)
            continue;
        if (procedure.report().failure)
            break;
        PhaseScope phaseScope(procedure, phase.name);
        bool changed = phase.run->run(procedure);
        RELEASE_ASSERT(point != ClientPhase::Point::BeforeLowerToAir || !changed);
    }
    return { };
}

static constexpr Step steps[] = {
    { "dumpInitialGraph"_s,
        [](Procedure& procedure, const PipelineState&) { return (shouldDumpIR(procedure, B3Mode) || Options::dumpGraphAfterParsing()) && !shouldDumpIRAtEachPhase(B3Mode); },
        [](Procedure& procedure, PipelineState&) -> Cleanups {
            dataLog(tierName, "Initial B3:\n");
            dataLog(procedure);
            return { };
        } },
    // We don't require the incoming IR to have predecessors computed.
    { "resetReachability"_s, always,
        [](Procedure& procedure, PipelineState&) -> Cleanups {
            procedure.resetReachability();
            if (Options::dumpIonGraph()) [[unlikely]]
                procedure.appendIonGraphPass("InitialCFG"_s);
            return { };
        } },
    { "validate"_s,
        [](Procedure& procedure, const PipelineState&) { return shouldValidateIR(procedure); },
        runValidation },
    { "checkKnownProperties"_s,
        [](Procedure& procedure, const PipelineState&) { return procedure.policy().validation.checksKnownPropertiesAtRunTime; },
        runPass<checkKnownProperties> },
    { "splitBranchConditions"_s,
        [](Procedure& procedure, const PipelineState&) { return procedure.policy().optimization.splitsBranchConditions; },
        runPass<splitBranchConditions> },
    { "estimateFrequencies"_s,
        [](Procedure& procedure, const PipelineState&) { return procedure.policy().optimization.estimatesFrequencies; },
        runNothing, NeededEstimate::EdgeClasses },
    { "clientPhasesBeforeOptimization"_s,
        [](Procedure& procedure, const PipelineState&) { return hasClientPhase(procedure, ClientPhase::Point::BeforeOptimization); },
        runClientPhases<ClientPhase::Point::BeforeOptimization> },
    { "validate"_s,
        [](Procedure& procedure, const PipelineState&) { return hasClientPhase(procedure, ClientPhase::Point::BeforeOptimization) && shouldValidateIR(procedure); },
        runValidation },
    { "checkVariablesAreSet"_s,
        [](Procedure& procedure, const PipelineState&) { return procedure.policy().optimization.checksVariablesAreSet; },
        runPass<checkVariablesAreSet> },
    { "reduceDoubleToFloat"_s,
        [](Procedure& procedure, const PipelineState&) { return procedure.optLevel() >= 2; },
        runPass<reduceDoubleToFloat> },
    { "specializeSelect"_s,
        [](Procedure& procedure, const PipelineState&) { return procedure.optLevel() >= 2 && procedure.policy().upstream.specializesSelects; },
        runPass<specializeSelect> },
    { "reduceStrength"_s,
        [](Procedure& procedure, const PipelineState&) { return procedure.optLevel() >= 1; },
        runPass<reduceStrength, ReduceStrengthPass::Initial> },
    { "hoistLoopInvariantValues"_s,
        [](Procedure& procedure, const PipelineState&) { return procedure.optLevel() >= 2 && hoistsInvariantValues(procedure) && !procedure.policy().loops.rotates; },
        runPass<hoistLoopInvariantValues> },
    { "eliminateCommonSubexpressions"_s,
        [](Procedure& procedure, const PipelineState&) { return procedure.optLevel() >= 2 && procedure.policy().optimization.redundancyElimination == RedundancyElimination::CommonSubexpressions; },
        [](Procedure& procedure, PipelineState&) -> Cleanups {
            eliminateCommonSubexpressions(procedure);
            return Cleanup::EliminateDeadCode;
        } },
    // Wasm has br_table instruction, so intent of the switch is already represented
    // and OMG generates Switch nodes directly.
    // In JS, switch can involve variables (non-constant values) and bytecode can fail
    // to be in op_switch. Thus after FTL with type speculation & constant folding,
    // we can infer a switch from if-else chain.
    { "inferSwitches"_s,
        [](Procedure& procedure, const PipelineState&) { return procedure.optLevel() >= 2 && !procedure.isWasm(); },
        runPass<inferSwitches> },
    { "threadJumps"_s,
        [](Procedure& procedure, const PipelineState&) { return procedure.optLevel() >= 2 && procedure.policy().optimization.threadsJumps; },
        runPass<threadJumps> },
    { "duplicateTails"_s,
        [](Procedure& procedure, const PipelineState&) { return procedure.optLevel() >= 2 && procedure.policy().upstream.duplicatesTails && !procedure.policy().optimization.threadsJumps; },
        runPass<duplicateTails> },
    { "fixSSA"_s, always, runPass<fixSSA> },
    { "foldPathConstants"_s,
        [](Procedure& procedure, const PipelineState&) { return procedure.optLevel() >= 2; },
        runPass<foldPathConstants> },
    { "findLoops"_s,
        [](Procedure& procedure, const PipelineState&) {
            const LoopPolicy& loops = procedure.policy().loops;
            return procedure.optLevel() >= 2 && (loops.rotates || loops.foldsComparisonsWithBounds || loops.versions);
        },
        [](Procedure& procedure, PipelineState& state) -> Cleanups {
            state.hasLoops = procedure.hasCycle() && procedure.naturalLoops().numLoops();
            return { };
        },
        NeededEstimate::EdgeClasses },
    { "simplifyTerminals"_s,
        [](Procedure&, const PipelineState& state) { return state.hasLoops; },
        runPass<simplifyTerminals> },
    { "eliminateRedundantPhiCycles"_s,
        [](Procedure&, const PipelineState& state) { return state.hasLoops; },
        runPass<eliminateRedundantPhiCycles> },
    { "hoistLoopInvariantValues"_s,
        [](Procedure& procedure, const PipelineState& state) { return state.hasLoops && hoistsInvariantValues(procedure); },
        runPass<hoistLoopInvariantValues> },
    { "rotateLoops"_s,
        [](Procedure& procedure, const PipelineState& state) { return state.hasLoops && procedure.policy().loops.rotates; },
        [](Procedure& procedure, PipelineState& state) {
            state.didRotateLoops = rotateLoops(procedure);
            return cleanupsIfChanged(state.didRotateLoops, Cleanup::FixSSA);
        } },
    { "hoistLoopInvariantValues"_s,
        [](Procedure& procedure, const PipelineState& state) { return state.didRotateLoops && hoistsInvariantValues(procedure); },
        runPass<hoistLoopInvariantValues> },
    { "cleanUpRotatedLoops"_s,
        [](Procedure&, const PipelineState& state) { return state.didRotateLoops; },
        [](Procedure&, PipelineState&) -> Cleanups { return { Cleanup::FoldPathConstants, Cleanup::SimplifyTerminals, Cleanup::EliminateDeadCode }; } },
    { "foldComparisonsWithBounds"_s,
        [](Procedure& procedure, const PipelineState& state) { return state.hasLoops && procedure.policy().loops.foldsComparisonsWithBounds; },
        [](Procedure& procedure, PipelineState&) { return cleanupsIfChanged(foldComparisonsWithBounds(procedure), { Cleanup::SimplifyTerminals, Cleanup::EliminateDeadCode }); } },
    { "versionLoops"_s,
        [](Procedure& procedure, const PipelineState& state) { return state.hasLoops && procedure.policy().loops.versions && hoistsInvariantValues(procedure); },
        [](Procedure& procedure, PipelineState& state) {
            state.didVersionLoops = versionLoops(procedure, state.topTestedVersionHeaders);
            return cleanupsIfChanged(state.didVersionLoops, Cleanup::FixSSA);
        } },
    { "eliminateRedundantPhiCycles"_s,
        [](Procedure&, const PipelineState& state) { return state.didVersionLoops; },
        [](Procedure& procedure, PipelineState&) -> Cleanups {
            eliminateRedundantPhiCycles(procedure);
            return Cleanup::EliminateDeadCode;
        } },
    { "rotateLoops"_s,
        [](Procedure& procedure, const PipelineState& state) { return state.didVersionLoops && procedure.policy().loops.rotates && !state.topTestedVersionHeaders.isEmpty(); },
        [](Procedure& procedure, PipelineState& state) {
            return cleanupsIfChanged(rotateLoops(procedure, &state.topTestedVersionHeaders), { Cleanup::FixSSA, Cleanup::FoldPathConstants, Cleanup::SimplifyTerminals, Cleanup::EliminateDeadCode });
        } },
    { "eliminateWasmGCAllocations"_s,
        [](Procedure& procedure, const PipelineState&) { return procedure.optLevel() >= 2 && procedure.usesWasmGCStructAllocations() && procedure.policy().upstream.eliminatesWasmGCAllocations; },
        runPass<eliminateWasmGCAllocations> },
    // This puts the IR in quirks mode.
    { "lowerMacros"_s, always, runPass<lowerMacros> },
    { "optimizeAssociativeExpressionTrees"_s,
        [](Procedure& procedure, const PipelineState&) { return procedure.optLevel() >= 2; },
        runPass<optimizeAssociativeExpressionTrees> },
    { "reduceStrength"_s,
        [](Procedure& procedure, const PipelineState&) { return procedure.optLevel() >= 2; },
        runPass<reduceStrength, ReduceStrengthPass::Final> },
    { "eliminateRedundantPhiCycles"_s,
        [](Procedure& procedure, const PipelineState&) { return procedure.optLevel() >= 2 && procedure.policy().optimization.convertsBranchesToSelects; },
        runLoopPass<eliminateRedundantPhiCycles> },
    { "convertBranchesToSelects"_s,
        [](Procedure& procedure, const PipelineState&) { return procedure.optLevel() >= 2 && procedure.policy().optimization.convertsBranchesToSelects; },
        runLoopPass<convertBranchesToSelects> },
    { "mergeAdjacentConstantStores"_s,
        [](Procedure& procedure, const PipelineState&) { return procedure.optLevel() >= 2 && procedure.policy().optimization.mergesAdjacentConstantStores; },
        runPass<mergeAdjacentConstantStores> },
    { "clientPhasesAfterOptimization"_s,
        [](Procedure& procedure, const PipelineState&) { return hasClientPhase(procedure, ClientPhase::Point::AfterOptimization); },
        runClientPhases<ClientPhase::Point::AfterOptimization> },
    { "validate"_s,
        [](Procedure& procedure, const PipelineState&) { return hasClientPhase(procedure, ClientPhase::Point::AfterOptimization) && shouldValidateIR(procedure); },
        runValidation },
    { "lowerMacrosAfterOptimizations"_s, always, runPass<lowerMacrosAfterOptimizations> },
    { "lowerRelocatableValues"_s, always, runPass<lowerRelocatableValues> },
    { "materializePinnedConstants"_s,
        [](Procedure& procedure, const PipelineState&) { return !!procedure.policy().addressing.findPinnedConstant; },
        runPass<materializePinnedConstants> },
    { "legalizeMemoryOffsets"_s, always, runPass<legalizeMemoryOffsets> },
    { "sinkValues"_s,
        [](Procedure& procedure, const PipelineState&) { return procedure.optLevel() >= 2 && procedure.policy().optimization.sinksValues; },
        runPass<sinkValues>, NeededEstimate::BlockFrequencies },
    { "clientPhasesAfterSinking"_s,
        [](Procedure& procedure, const PipelineState&) { return hasClientPhase(procedure, ClientPhase::Point::AfterSinking); },
        runClientPhases<ClientPhase::Point::AfterSinking> },
    { "moveConstants"_s, always, runPass<moveConstants>, NeededEstimate::BlockFrequencies },
    { "lowerRelocatableValues"_s,
        [](Procedure& procedure, const PipelineState&) { return !procedure.dataSections().empty(); },
        runPass<lowerRelocatableValues> },
    { "legalizeMemoryOffsets"_s, always, runPass<legalizeMemoryOffsets> },
    { "eliminateDeadCode"_s, always, runPass<eliminateDeadCode> },
    { "canonicalizePrePostIncrements"_s,
        [](Procedure& procedure, const PipelineState&) { return procedure.optLevel() >= 2 && procedure.policy().upstream.canonicalizesIncrements; },
        runPass<canonicalizePrePostIncrements> },
    // FIXME: We should run pureCSE here to clean up some platform specific changes from the previous phases.
    // https://bugs.webkit.org/show_bug.cgi?id=164873
    { "validate"_s,
        [](Procedure& procedure, const PipelineState&) { return shouldValidateIR(procedure); },
        runValidation },
    // If we're doing super verbose dumping, the phase scope of any phase will already do a dump.
    // Note that lowerToAir() acts like a phase in this regard.
    { "dumpFinalGraph"_s,
        [](Procedure& procedure, const PipelineState&) { return shouldDumpIR(procedure, B3Mode) && !shouldDumpIRAtEachPhase(B3Mode); },
        [](Procedure& procedure, PipelineState&) -> Cleanups {
            dataLog("B3 after ", procedure.lastPhaseName(), ", before generation:\n");
            dataLog(procedure);
            return { };
        } },
    { "clientPhasesBeforeLowerToAir"_s,
        [](Procedure& procedure, const PipelineState&) { return hasClientPhase(procedure, ClientPhase::Point::BeforeLowerToAir); },
        runClientPhases<ClientPhase::Point::BeforeLowerToAir> },
    { "lowerToAir"_s, always,
        [](Procedure& procedure, PipelineState&) -> Cleanups {
            lowerToAir(procedure);
            if (shouldDumpIR(procedure, B3Mode))
                procedure.setShouldDumpIR();
            procedure.freeUnneededB3ValuesAfterLowering();
            return { };
        },
        NeededEstimate::BlockFrequencies },
};

void runCleanups(std::span<Procedure* const> procedures, std::span<PipelineState> states)
{
    struct CleanupPass {
        Cleanup cleanup;
        void (*run)(Procedure&);
    };
    static constexpr CleanupPass cleanupPasses[] = {
        { Cleanup::FixSSA, [](Procedure& procedure) { fixSSA(procedure); } },
        { Cleanup::FoldPathConstants, [](Procedure& procedure) { foldPathConstants(procedure); } },
        { Cleanup::SimplifyTerminals, [](Procedure& procedure) { simplifyTerminals(procedure); } },
        { Cleanup::EliminateDeadCode, [](Procedure& procedure) { eliminateDeadCode(procedure); } },
    };

    ASSERT(procedures.size() == states.size());
    for (const CleanupPass& cleanupPass : cleanupPasses) {
        for (size_t index = 0; index < procedures.size(); ++index) {
            Procedure& procedure = *procedures[index];
            if (states[index].pendingCleanups.contains(cleanupPass.cleanup) && !procedure.report().failure)
                cleanupPass.run(procedure);
        }
    }

    for (PipelineState& state : states)
        state.pendingCleanups = { };
}

void runSteps(std::span<Procedure* const> procedures)
{
    CompilerTimingScope timingScope("Total B3"_s, "generateToAir"_s);

    Vector<PipelineState, 16> states(procedures.size());
    for (const Step& step : steps) {
        bool hasPendingCleanups = false;
        for (size_t index = 0; index < procedures.size(); ++index) {
            Procedure& procedure = *procedures[index];
            PipelineState& state = states[index];
            if (procedure.report().failure || !step.isEnabled(procedure, state))
                continue;
            if (step.neededEstimate == NeededEstimate::EdgeClasses)
                procedure.updateEdgeClasses();
            else if (step.neededEstimate == NeededEstimate::BlockFrequencies)
                procedure.updateFrequencies();
            state.pendingCleanups = step.run(procedure, state);
            hasPendingCleanups |= !state.pendingCleanups.isEmpty();
        }
        if (hasPendingCleanups)
            runCleanups(procedures, states.mutableSpan());
    }
}

} // anonymous namespace

void prepareForGeneration(Procedure& procedure)
{
    Procedure* procedures[] = { &procedure };
    prepareForGeneration(std::span<Procedure* const>(procedures));
}

void prepareForGeneration(std::span<Procedure* const> procedures)
{
    if (procedures.empty())
        return;

    CompilerTimingScope timingScope("Total B3+Air"_s, "prepareForGeneration"_s);

    runSteps(procedures);

    Vector<Air::Code*, 16> codes;
    for (Procedure* procedure : procedures) {
        if (!procedure->report().failure)
            codes.append(&procedure->code());
    }
    Air::prepareForGeneration(codes.span());
}

void generate(Procedure& procedure, CCallHelpers& jit)
{
    Air::generate(procedure.code(), jit);
}

void generateToAir(Procedure& procedure)
{
    Procedure* procedures[] = { &procedure };
    runSteps(std::span<Procedure* const>(procedures));
}

Vector<ASCIILiteral> listEnabledSteps(Procedure& procedure)
{
    Vector<ASCIILiteral> result;
    PipelineState state;
    for (const Step& step : steps) {
        if (step.isEnabled(procedure, state))
            result.append(step.name);
    }
    return result;
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
