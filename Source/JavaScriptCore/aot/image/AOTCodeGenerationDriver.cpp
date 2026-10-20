/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCodeGenerationDriver.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCodeUnits.h"
#include "AOTCompileFunctions.h"
#include "AOTFlowResults.h"
#include "AOTIRProgram.h"
#include "AOTIdentifierNumbering.h"
#include "AOTPipeline.h"
#include "AOTPlacement.h"
#include "AOTRegExpRequests.h"
#include "AOTRemoveUnreachableFunctions.h"
#include "AOTSiteCachePlacement.h"
#include "AOTSlotPlan.h"
#include "AOTStubCatalog.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include <algorithm>

namespace JSC { namespace AOT {

static constexpr uint32_t maxFunctionsInBatch = 16;
static constexpr uint64_t maxEstimatedBytesInBatch = 12 * KB;

namespace {

struct Candidate {
    uint32_t size;
    uint32_t function;
};

struct Batch {
    uint32_t first;
    uint32_t size;
};

} // anonymous namespace

CodeGenerationPlan::CodeGenerationPlan() = default;
CodeGenerationPlan::CodeGenerationPlan(CodeGenerationPlan&&) = default;
CodeGenerationPlan::~CodeGenerationPlan() = default;

CodeGenerationPlan prepareCodeGeneration(const Typed::PipelineOutput& pipeline, BuildContext& context)
{
    const Typed::Program& program = *pipeline.program;

    CodeGenerationPlan plan;
    plan.uses = collectProgramUses(pipeline, context);

    IdentifierNumbering numbering = numberIdentifiers(program, plan.uses.keys, pipeline.slots->keysWithColumn(), context);
    StubPolicy policy = chooseStubPolicy(plan.uses.keys, numbering, context);
    addRegExpLiterals(policy, *pipeline.regExpRequests, pipeline.flow->mayReadLegacyRegExpStatics(), context);
    plan.stubCatalog = makeUniqueWithoutFastMallocCheck<const StubCatalog>(StubOptions::from(context.recordedOptions(), program), WTF::move(policy));
    context.setIdentifierNumbering(WTF::move(numbering));

    plan.stringConstants = numberStringConstants(program, plan.uses.strings, context);
    plan.instanceSizes = { pipeline.placement->sizeOfStatics(), plan.stringConstants.count(), pipeline.siteCaches->count() };
    if (!InstanceLayout::isAddressable(plan.instanceSizes))
        context.reportLimitExceeded(LimitID::MaxStaticsSize, std::max(InstanceLayout::bytesReservedBelowInstance(plan.instanceSizes), InstanceLayout::bytesReservedFromInstance(plan.instanceSizes)));

    plan.branchCounters = Typed::numberBranchCounters(program, context);
    return plan;
}

Typed::CodeGenerationInputs createCodeGenerationInputs(const Typed::PipelineOutput& pipeline, const CodeGenerationPlan& plan)
{
    return {
        .program = *pipeline.program,
        .placement = *pipeline.placement,
        .flow = *pipeline.flow,
        .heat = *pipeline.heat,
        .effects = *pipeline.effects,
        .derivedFunctions = *pipeline.derivedFunctions,
        .conventions = *pipeline.callingConventions,
        .stackChecks = *pipeline.stackChecks,
        .frameAllocation = *pipeline.frameAllocation,
        .literalStrings = *pipeline.literalStrings,
        .stubCatalog = *plan.stubCatalog,
        .stringConstants = plan.stringConstants,
        .instanceSizes = plan.instanceSizes,
        .branchCounters = plan.branchCounters,
    };
}

static Vector<Candidate> collectCandidates(const Typed::Program& program, const BuildContext& context)
{
    const ParallelLoops& loops = context.parallelLoops();
    BitVector hasNoCode = Typed::findBodiesWithoutCode(program, context);
    Vector<uint32_t> functionsWithCode = findIndicesInParallel(loops, program.functions.size(), [&](unsigned index) {
        return program.functions[index].hasBody() && !hasNoCode.get(index);
    });
    Vector<Candidate> largestFirst(functionsWithCode.size());
    forEachIndexInParallel(loops, largestFirst.size(), [&](unsigned index) {
        uint32_t function = functionsWithCode[index];
        largestFirst[index] = { Typed::estimatedCodeSize(program, FunctionID(function)), function };
    });
    parallelSort(loops, largestFirst.mutableSpan(), [](const Candidate& a, const Candidate& b) {
        return a.size != b.size ? a.size > b.size : a.function < b.function;
    });
    return largestFirst;
}

static Vector<Batch> createBatches(std::span<const Candidate> largestFirst)
{
    Vector<Batch> batches;
    uint64_t estimatedBytesInBatch = 0;
    for (uint32_t index = 0; index < largestFirst.size(); ++index) {
        uint64_t estimatedBytes = largestFirst[index].size;
        bool joinsOpenBatch = !batches.isEmpty()
            && batches.last().size < maxFunctionsInBatch
            && estimatedBytesInBatch + estimatedBytes <= maxEstimatedBytesInBatch;
        if (!joinsOpenBatch) {
            batches.append(Batch { index, 0 });
            estimatedBytesInBatch = 0;
        }
        ++batches.last().size;
        estimatedBytesInBatch += estimatedBytes;
    }
    return batches;
}

void dropUnitsWithoutOutlinableCode(CodeBody& body)
{
    bool hasOutlinableCode = std::ranges::any_of(body.units, [](const CodeUnit& unit) {
        return unit.kind == CodeUnit::Kind::Plain;
    });
    if (hasOutlinableCode)
        return;
    body.units = { };
    body.localSites = { };
}

static void shrinkToFit(CompiledCode& code)
{
    auto& body = code.body;
    body.bytes.shrinkToFit();
    body.entries.shrinkToFit();
    body.units.shrinkToFit();
    body.localSites.shrinkToFit();
    body.relocations.shrinkToFit();
    body.returnSites.shrinkToFit();
    body.firstReadSites.shrinkToFit();
    body.callees.shrinkToFit();
    body.stubKeys.shrinkToFit();
    body.argumentMoves.shrinkToFit();
    body.constants.shrinkToFit();
    body.jumpTables.shrinkToFit();
    code.frame.frameRanges.shrinkToFit();
    code.frame.prologueChecks.shrinkToFit();
    auto& sites = code.sites;
    sites.positions.shrinkToFit();
    sites.inlineFrames.shrinkToFit();
    sites.spreadSites.shrinkToFit();
    sites.flaggedSites.shrinkToFit();
    sites.handlers.shrinkToFit();
    sites.faultingAccesses.shrinkToFit();
    sites.pollSites.shrinkToFit();
    sites.frameMapWords.shrinkToFit();
}

static void prepareForImage(CompiledCode& code, BuildContext& context)
{
    if (context.validates()) {
        CodeUnitsError error = verifyCodeUnits(code);
        if (error != CodeUnitsError::None)
            context.diagnostics().add(InternalError { .kind = InternalErrorKind::InvalidCodeUnits, .function = code.function, .count = static_cast<uint32_t>(error) });
    }
    context.statistics().count(StatisticID::CodeUnitsRecorded, code.body.units.size());
    dropUnitsWithoutOutlinableCode(code.body);
    shrinkToFit(code);
}

static void reportFunctionsWithoutCode(const LinkedFunctions& functions, BuildContext& context)
{
    if (context.diagnostics().hasErrors())
        return;
    Vector<uint32_t> functionsWithoutCode = findIndicesInParallel(context.parallelLoops(), functions.size(), [&](unsigned index) {
        return functions[index].body.bytes.isEmpty();
    });
    for (uint32_t index : functionsWithoutCode)
        context.diagnostics().add(InternalError { .kind = InternalErrorKind::FunctionWithoutCode, .function = functions[index].function });
}

GeneratedCode generateAllCode(const Typed::CodeGenerationInputs& inputs, Typed::TypeTable& types, BuildContext& context)
{
    const ParallelLoops& loops = context.parallelLoops();
    Vector<Candidate> largestFirst = collectCandidates(inputs.program, context);
    Vector<Batch> batches = createBatches(largestFirst.span());
    Vector<FunctionID> ids(largestFirst.size(), [&](size_t index) {
        return FunctionID(largestFirst[index].function);
    });

    GeneratedCode result;
    result.functions.grow(ids.size(), context);
    std::span<CompiledCode> functions = result.functions.mutableSpan();
    Vector<CodeGenerationTotals> totals(context.threadCount());
    Typed::TypeInterningPhase phase(types, batches.size());
    forEachIndexInParallel(loops, batches.size(), [&](unsigned index, unsigned worker) {
        Batch batch = batches[index];
        std::span<const FunctionID> idsInBatch = ids.subspan(batch.first, batch.size);
        std::span<CompiledCode> functionsInBatch = functions.subspan(batch.first, batch.size);
        Typed::TypeArena arena(phase, index);
        Typed::compileFunctions({ context, arena, context.backEndScratch(worker) }, inputs, idsInBatch, functionsInBatch, totals[worker]);
        for (uint32_t member = 0; member < batch.size; ++member) {
            auto& code = functionsInBatch[member];
            if (code.body.bytes.isEmpty())
                code.function = idsInBatch[member];
            else
                prepareForImage(code, context);
        }
    });
    phase.discard();

    for (auto& workerTotals : totals)
        result.entriesUsed.merge(workerTotals.entriesUsed);
    reportFunctionsWithoutCode(result.functions, context);
    return result;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
