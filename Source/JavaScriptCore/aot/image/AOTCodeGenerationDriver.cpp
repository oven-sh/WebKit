/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCodeGenerationDriver.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCFG.h"
#include "AOTCodeUnits.h"
#include "AOTCompileFunctions.h"
#include "AOTExactBuffer.h"
#include "AOTFlowResults.h"
#include "AOTIRProgram.h"
#include "AOTIdentifierNumbering.h"
#include "AOTPipeline.h"
#include "AOTPlacement.h"
#include "AOTProgramCaches.h"
#include "AOTRegExpRequests.h"
#include "AOTRemoveUnreachableFunctions.h"
#include "AOTSiteCachePlacement.h"
#include "AOTSlotPlan.h"
#include "AOTStubCatalog.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>

namespace JSC { namespace AOT {

AOT_THRESHOLD(uint32_t, maxFunctionsInBatch, 16);
AOT_THRESHOLD(uint64_t, maxEstimatedBytesInBatch, 12 * KB);

namespace {

struct Candidate {
    uint32_t size;
    uint32_t function;
};

struct Candidates {
    Vector<Candidate> largestFirst;
    Vector<uint32_t> functionsWithoutCode;
};

struct Batch {
    uint32_t first;
    uint32_t size;
};

class BodyReleaser {
public:
    BodyReleaser(Typed::Program&, Vector<OperationCounts>&, BuildContext&);

    void willRelease(FunctionID);
    void hideUntilCompiled(std::span<const FunctionID>, const ParallelLoops&);
    void show(size_t position, FunctionID);
    void release(FunctionID);

private:
    struct Body {
        Typed::Function::Blocks::Storage blocks;
        Typed::Function::Values::Storage values;
    };

    Typed::Program& m_program;
    Vector<OperationCounts>& m_operationCountsByFunction;
    BuildContext& m_context;
    BitVector m_isNamedByLoss;
    Vector<Body> m_hiddenBodies;
};

} // anonymous namespace

CodeGenerationPlan::CodeGenerationPlan() = default;
CodeGenerationPlan::CodeGenerationPlan(CodeGenerationPlan&&) = default;
CodeGenerationPlan::~CodeGenerationPlan() = default;

CodeGenerationPlan prepareCodeGeneration(const Typed::PipelineOutput& pipeline, BuildContext& context)
{
    const Typed::Program& program = *pipeline.program;

    CodeGenerationPlan plan;
    plan.bodiesWithoutCode = Typed::findBodiesWithoutCode(program, context, pipeline.calledFunctions.span());
    const BitVector& bodiesWithoutCode = plan.bodiesWithoutCode;
    for (size_t index : pipeline.placement->bodiesWithoutCodeWhenPlaced()) {
        if (program.functions[index].hasBody() && !bodiesWithoutCode.get(index))
            context.diagnostics().add(InternalError { .kind = InternalErrorKind::PlacementCheckFailed, .function = FunctionID(index) });
    }
    runTasksInParallel(context.parallelLoops(), {
        [&] {
            plan.uses = collectProgramUses(pipeline, bodiesWithoutCode, context);

            IdentifierNumbering numbering = numberIdentifiers(program, plan.uses.keys, pipeline.slots->keysWithColumn(), context);
            StubPolicy policy = chooseStubPolicy(plan.uses.keys, numbering, context);
            addRegExpLiterals(policy, *pipeline.regExpRequests, pipeline.flow->mayReadLegacyRegExpStatics(), context);
            plan.stubCatalog = makeUniqueWithoutFastMallocCheck<const StubCatalog>(StubOptions::from(context.recordedOptions(), program, *pipeline.flow), WTF::move(policy));
            context.setIdentifierNumbering(WTF::move(numbering));

            plan.stringConstants = numberStringConstants(program, plan.uses.strings, context);
            plan.instanceSizes = { pipeline.placement->sizeOfStatics(), plan.stringConstants.count(), pipeline.siteCaches->count(), static_cast<uint32_t>(pipeline.placement->methodTable().sizeInBytes()) };
            if (!InstanceLayout::isAddressable(plan.instanceSizes))
                context.reportLimitExceeded(LimitID::MaxStaticsSize, std::max(InstanceLayout::bytesReservedBelowInstance(plan.instanceSizes), InstanceLayout::bytesReservedFromInstance(plan.instanceSizes)));
        },
        [&] { plan.bodyFactsForTables = Typed::collectBodyFactsForTables(program, bodiesWithoutCode, context); },
        [&] {
            plan.branchCounters = Typed::numberBranchCounters(program, context);
            plan.arrivalSites = Typed::numberArrivalSites(program, context);
        },
        [&] { plan.promisedResults = Typed::findPromisedResults(program, *pipeline.callingConventions, context.parallelLoops()); },
    });
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
        .arrivalSites = plan.arrivalSites,
        .promisedResults = plan.promisedResults,
    };
}

static Candidates collectCandidates(const Typed::Program& program, const BuildContext& context, const BitVector* bodiesWithoutCode)
{
    const ParallelLoops& loops = context.parallelLoops();
    BitVector hasNoCode = bodiesWithoutCode ? *bodiesWithoutCode : Typed::findBodiesWithoutCode(program, context);
    RELEASE_ASSERT(!bodiesWithoutCode || !context.validates() || hasNoCode == Typed::findBodiesWithoutCode(program, context));
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
    Vector<uint32_t> functionsWithoutCode;
    for (size_t index : hasNoCode)
        functionsWithoutCode.append(static_cast<uint32_t>(index));
    return { WTF::move(largestFirst), WTF::move(functionsWithoutCode) };
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
    for (CodeUnit unit : body.units) {
        if (unit.isPlain())
            return;
    }
    body.units = { };
    // The code of a body that calls nothing and jumps on to one of several functions may be laid in another order: endInJumpToNextBody().
    if (!body.returnSites.isEmpty() || body.relocations.size() < 2)
        body.localSites = { };
}

static void shrinkToFit(CompiledCode& code)
{
    auto& body = code.body;
    moveToExactBuffer(body.bytes);
    moveToExactBuffer(body.entries);
    moveToExactBuffer(body.localSites);
    moveToExactBuffer(body.relocations);
    moveToExactBuffer(body.returnSites);
    moveToExactBuffer(body.firstReadSites);
    moveToExactBuffer(body.callees);
    moveToExactBuffer(body.stubKeys);
    moveToExactBuffer(body.argumentMoves);
    moveToExactBuffer(body.constants);
    moveToExactBuffer(body.jumpTables);
    moveToExactBuffer(code.frame.frameRanges);
    moveToExactBuffer(code.frame.prologueChecks);
    auto& sites = code.sites;
    moveToExactBuffer(sites.positions);
    moveToExactBuffer(sites.inlineFrames);
    moveToExactBuffer(sites.spreadSites);
    moveToExactBuffer(sites.flaggedSites);
    moveToExactBuffer(sites.handlers);
    moveToExactBuffer(sites.faultingAccesses);
    moveToExactBuffer(sites.pollSites);
    moveToExactBuffer(sites.frameMapWords);
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

OperationCounts countOperations(const Typed::Function& function)
{
    if (function.blocks.isEmpty())
        return { };

    std::array<uint32_t, numberOfEnumerators<Typed::Opcode>> counts { };
    Typed::CFG cfg(function);
    for (Typed::BlockID block : cfg.reversePostOrder()) {
        for (Typed::ValueID value : function.at(block).values)
            ++counts[static_cast<size_t>(function.at(value).opcode)];
    }

    OperationCounts operations;
    for (Typed::Opcode opcode : allMembersOf<Typed::Opcode>) {
        if (uint32_t count = counts[static_cast<size_t>(opcode)])
            operations.append({ opcode, count });
    }
    return operations;
}

BodyReleaser::BodyReleaser(Typed::Program& program, Vector<OperationCounts>& operationCountsByFunction, BuildContext& context)
    : m_program(program)
    , m_operationCountsByFunction(operationCountsByFunction)
    , m_context(context)
    , m_isNamedByLoss(program.functions.size())
{
    if (hasFlag(context.options().reports, ReportSet::Functions))
        m_operationCountsByFunction.grow(program.functions.size());
    context.diagnostics().resolveLocations(program);
    for (FunctionID function : context.functionsWithUnresolvedWitnesses()) {
        if (function.index() < program.functions.size())
            m_isNamedByLoss.quickSet(function.index());
    }
}

// Other threads ask hasBody() while a body is released, so the flag is set before any thread releases one.
void BodyReleaser::willRelease(FunctionID id)
{
    Typed::Function& function = m_program.at(id);
    // A validating build reads models while it compiles their callers.
    function.bodyIsReleased = function.role != Typed::FunctionRole::Model && !m_isNamedByLoss.get(id.index());
}

// Whether a thread finds the body of another function would depend on timing, so it never does.
void BodyReleaser::hideUntilCompiled(std::span<const FunctionID> ids, const ParallelLoops& loops)
{
    m_hiddenBodies.grow(ids.size());
    forEachIndexInParallel(loops, static_cast<unsigned>(ids.size()), [&](unsigned position) {
        Typed::Function& function = m_program.at(ids[position]);
        if (function.bodyIsReleased)
            m_hiddenBodies[position] = { function.takeBlocks(), function.takeValues() };
    });
}

void BodyReleaser::show(size_t position, FunctionID id)
{
    Typed::Function& function = m_program.at(id);
    if (!function.bodyIsReleased)
        return;
    Body body = WTF::move(m_hiddenBodies[position]);
    function.editAllBlocks() = WTF::move(body.blocks);
    function.editAllValues() = WTF::move(body.values);
}

void BodyReleaser::release(FunctionID id)
{
    Typed::Function& function = m_program.at(id);
    if (!function.bodyIsReleased || m_context.diagnostics().hasErrors())
        return;
    if (!m_operationCountsByFunction.isEmpty())
        m_operationCountsByFunction[id.index()] = countOperations(function);
    function.releaseBody();
}

struct CodeGenerationJob::State {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(State);

    State(const Typed::CodeGenerationInputs& inputs, Typed::Program& program, BuildContext& context)
        : inputs(inputs)
        , program(program)
        , context(context)
        , releaser(program, result.operationCountsByFunction, context)
    {
    }

    const Typed::CodeGenerationInputs& inputs;
    Typed::Program& program;
    BuildContext& context;
    Vector<Batch> batches;
    Vector<FunctionID> ids;
    GeneratedCode result;
    BodyReleaser releaser;
};

CodeGenerationJob::CodeGenerationJob(const Typed::CodeGenerationInputs& inputs, Typed::Program& program, BuildContext& context, const BitVector* bodiesWithoutCode)
    : m_state(makeUnique<State>(inputs, program, context))
{
    RELEASE_ASSERT(&program == &inputs.program);
    auto [largestFirst, functionsWithoutCode] = collectCandidates(program, context, bodiesWithoutCode);
    m_state->batches = createBatches(largestFirst.span());
    m_state->ids = Vector<FunctionID>(largestFirst.size(), [&](size_t index) {
        return FunctionID(largestFirst[index].function);
    });

    BodyReleaser& releaser = m_state->releaser;
    const ParallelLoops& loops = context.parallelLoops();
    forEachIndexInParallel(loops, functionsWithoutCode.size(), [&](unsigned index) {
        releaser.willRelease(FunctionID(functionsWithoutCode[index]));
    });
    forEachIndexInParallel(loops, m_state->ids.size(), [&](unsigned index) {
        releaser.willRelease(m_state->ids[index]);
    });
    releaser.hideUntilCompiled(m_state->ids.span(), loops);
    forEachIndexInParallel(loops, functionsWithoutCode.size(), [&](unsigned index) {
        releaser.release(FunctionID(functionsWithoutCode[index]));
    });
}

CodeGenerationJob::~CodeGenerationJob() = default;

GeneratedCode CodeGenerationJob::run()
{
    auto& [inputs, program, context, batches, ids, result, releaser] = *m_state;
    const ParallelLoops& loops = context.parallelLoops();
    result.functions.grow(ids.size(), context);
    std::span<CompiledCode> functions = result.functions.mutableSpan();
    Vector<CodeGenerationTotals> totals(context.threadCount());
    Typed::TypeInterningPhase phase(program.types, batches.size());
    {
        Typed::ProgramCaches caches(inputs.program, context);
        forEachIndexInParallel(loops, batches.size(), [&](unsigned index, unsigned worker) {
            Batch batch = batches[index];
            std::span<const FunctionID> idsInBatch = ids.subspan(batch.first, batch.size);
            std::span<CompiledCode> functionsInBatch = functions.subspan(batch.first, batch.size);
            for (uint32_t member = 0; member < batch.size; ++member)
                releaser.show(batch.first + member, idsInBatch[member]);
            Typed::TypeArena arena(phase, index);
            Typed::compileFunctions({ context, arena, context.backEndScratch(worker) }, inputs, idsInBatch, functionsInBatch, totals[worker]);
            for (uint32_t member = 0; member < batch.size; ++member) {
                auto& code = functionsInBatch[member];
                if (code.body.bytes.isEmpty())
                    code.function = idsInBatch[member];
                else
                    prepareForImage(code, context);
            }
            for (FunctionID id : idsInBatch)
                releaser.release(id);
        });
    }
    phase.discard();

    for (auto& workerTotals : totals)
        result.entriesUsed.merge(workerTotals.entriesUsed);
    reportFunctionsWithoutCode(result.functions, context);
    return WTF::move(result);
}

GeneratedCode generateAllCode(const Typed::CodeGenerationInputs& inputs, Typed::Program& program, BuildContext& context)
{
    return CodeGenerationJob(inputs, program, context).run();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
