/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCodeGenerationDriver.h"
#include "AOTImagesBelow.h"

#if ENABLE(AOT)

#include "AOTCompileFunctions.h"
#include "AOTExactBuffer.h"
#include "AOTFlowResults.h"
#include "AOTIdentifierNumbering.h"
#include "AOTPipeline.h"
#include "AOTPlacement.h"
#include "AOTProgramCaches.h"
#include "AOTProgramTableEncoder.h"
#include "AOTRegExpRequests.h"
#include "AOTRemoveUnreachableFunctions.h"
#include "AOTSlotPlan.h"
#include "AOTTypeArena.h"
#include <optional>

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

} // anonymous namespace

CodeGenerationPlan prepareCodeGeneration(const Typed::PipelineOutput& pipeline, BuildContext& context, ProgramTablesUse tables)
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
            Typed::NamesInProgramTables namesInTables;
            runTasksInParallel(context.parallelLoops(), {
                [&] { plan.uses = collectProgramUses(pipeline, bodiesWithoutCode, context); },
                [&] {
                    plan.bodyFactsForTables = Typed::collectBodyFactsForTables(program, bodiesWithoutCode, context);
                    if (tables == ProgramTablesUse::Written)
                        namesInTables = Typed::findNamesInProgramTables(pipeline, plan.bodyFactsForTables, context);
                },
            });
            plan.uses.keys.keysNamedByTables = WTF::move(namesInTables.keys);
            plan.uses.keys.stringsNamedAsKeysByTables = WTF::move(namesInTables.texts);

            IdentifierNumbering numbering = numberIdentifiers(program, plan.uses.keys, pipeline.slots->keysWithColumn(), context);
            StubPolicy policy = chooseStubPolicy(plan.uses.keys, numbering, context);
            addExpectedFieldTableEntries(policy, program, *pipeline.slots, numbering);
            chooseOrderOfFieldTableForms(policy, program, *pipeline.slots, plan.uses.keys);
            addRegExpLiterals(policy, *pipeline.regExpRequests, pipeline.flow->mayReadLegacyRegExpStatics(), context);
            StubOptions stubOptions = StubOptions::from(context.recordedOptions(), program, *pipeline.flow);
            stubOptions.maxNameInFieldTable = numbering.countWithFieldTableColumn() - 1;
            if (context.options().sharesRealms()) {
                stubOptions.sharesRealms = true;
                stubOptions.imagesBelow = context.imagesBelow() ? context.imagesBelow()->count() : 0;
                stubOptions.shapesBelow = pipeline.placement->shapes().shapesBelow();
                stubOptions.lastShape = pipeline.placement->shapes().numberOfShapes();
                stubOptions.firstStringHeader = numbering.first();
                Typed::ClosureFunctionRange closures = pipeline.placement->closureFunctions();
                stubOptions.closureNumbersBelow = closures.numbersBelow;
                stubOptions.endOfClosureNumbers = closures.numbersBelow + closures.end.index() - closures.first.index();
                stubOptions.layoutIndicesBelow = context.imagesBelow() ? context.imagesBelow()->layoutIndices() : 0;
                stubOptions.numberOfLayoutIndices = static_cast<uint32_t>(program.layouts.size());
            }
            plan.stubCatalog = makeUniqueWithoutFastMallocCheck<const StubCatalog>(stubOptions, WTF::move(policy));
            context.setIdentifierNumbering(WTF::move(numbering));

            plan.stringConstants = numberStringConstants(program, plan.uses.strings, context);
            plan.instanceSizes = { pipeline.placement->sizeOfStatics(), plan.stringConstants.count(), pipeline.siteCaches->count(), static_cast<uint32_t>(pipeline.placement->methodTable().sizeInBytes()) };
            if (!InstanceLayout::isAddressable(plan.instanceSizes))
                context.reportLimitExceeded(LimitID::MaxStaticsSize, std::max(InstanceLayout::bytesReservedBelowInstance(plan.instanceSizes), InstanceLayout::bytesReservedFromInstance(plan.instanceSizes)));
        },
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

void LinkedFunctions::grow(size_t newSize, BuildContext& context)
{
    RELEASE_ASSERT(isEmpty());
    *this = LinkedFunctions(context.parallelLoops(), safeCast<unsigned>(newSize), [](unsigned) {
        return CompiledCode { };
    });
}

void LinkedFunctions::makeRoomFor(size_t newSize)
{
    RELEASE_ASSERT(isEmpty());
    static_cast<ParallelArray&>(*this) = ParallelArray::withRoomFor(safeCast<unsigned>(newSize));
}

void LinkedFunctions::clearOnAllThreads(BuildContext& context)
{
    forEachIndexInParallel(context.parallelLoops(), size(), [&](unsigned index) {
        CompiledCode code = WTF::move((*this)[index]);
    });
    *this = { };
}

void LinkedFunctions::reorder(std::span<const uint32_t> order, BuildContext& context)
{
    LinkedFunctions reordered(context.parallelLoops(), static_cast<unsigned>(order.size()), [&](unsigned index) {
        return WTF::move((*this)[order[index]]);
    });
    context.destroyLater(std::exchange(*this, WTF::move(reordered)));
}

static Candidates collectCandidates(const Typed::Program& program, BuildContext& context, const BitVector* bodiesWithoutCode)
{
    const ParallelLoops& loops = context.parallelLoops();
    BitVector hasNoCode = bodiesWithoutCode ? *bodiesWithoutCode : Typed::findBodiesWithoutCode(program, context);
    RELEASE_ASSERT(!bodiesWithoutCode || !context.validates() || hasNoCode == Typed::findBodiesWithoutCode(program, context));
    Vector<uint32_t> functionsWithCode = findIndicesInParallel(loops, program.functions.size(), [&](unsigned index) {
        return program.functions[index].hasBody() && !hasNoCode.get(index);
    });
    Vector<Candidate> largestFirst(functionsWithCode.size());
    Typed::ProgramCaches caches(program, context);
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
    auto shrink = [](auto&... vectors) {
        (moveToExactBuffer(vectors), ...);
    };
    auto& body = code.body;
    auto& sites = code.sites;
    shrink(body.bytes, body.entries, body.localSites, body.relocations, body.returnSites, body.firstReadSites, body.callees, body.stubKeys, body.argumentMoves, body.constants, body.jumpTables, body.smallLoops);
    shrink(code.frame.frameRanges, code.frame.prologueChecks);
    shrink(sites.positions, sites.inlineFrames, sites.flaggedSites, sites.handlers, sites.faultingAccesses, sites.pollSites, sites.frameMapWords);
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

struct CodeGenerationJob::State {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(State);

    void release(FunctionID id)
    {
        Typed::Function& function = program.at(id);
        if (!function.bodyIsReleased || context.diagnostics().hasErrors())
            return;
        if (!result.operationCountsByFunction.isEmpty())
            result.operationCountsByFunction[id.index()] = countOperations(function);
        function.releaseBody();
    }

    const Typed::CodeGenerationInputs& inputs;
    Typed::Program& program;
    BuildContext& context;
    Vector<Batch> batches;
    Vector<FunctionID> ids;
    Vector<Typed::ParkedBody> hiddenBodies;
    GeneratedCode result;
    std::optional<Typed::TypeInterningPhase> phase;
    std::optional<Typed::ProgramCaches> caches;
};

CodeGenerationJob::CodeGenerationJob(const Typed::CodeGenerationInputs& inputs, Typed::Program& program, BuildContext& context, const BitVector* bodiesWithoutCode)
    : m_state(makeUnique<State>(inputs, program, context))
{
    RELEASE_ASSERT(&program == &inputs.program);
    const ParallelLoops& loops = context.parallelLoops();
    if (hasFlag(context.options().reports, ReportSet::Functions))
        m_state->result.operationCountsByFunction.grow(program.functions.size());
    context.diagnostics().resolveLocations(program);
    BitVector isNamedByLoss(program.functions.size());
    for (FunctionID function : context.functionsWithUnresolvedWitnesses()) {
        if (function.index() < program.functions.size())
            isNamedByLoss.quickSet(function.index());
    }

    auto [largestFirst, functionsWithoutCode] = collectCandidates(program, context, bodiesWithoutCode);
    m_state->batches = createBatches(largestFirst.span());
    m_state->ids = Vector<FunctionID>(largestFirst.size(), [&](size_t index) {
        return FunctionID(largestFirst[index].function);
    });
    std::span<const FunctionID> ids = m_state->ids.span();
    // Other threads ask hasBody() while a body is released, so the flag is set before any thread releases one.
    auto willRelease = [&](FunctionID id) {
        Typed::Function& function = program.at(id);
        // A validating build reads models while it compiles their callers.
        function.bodyIsReleased = function.role != Typed::FunctionRole::Model && !isNamedByLoss.get(id.index());
    };
    forEachIndexInParallel(loops, functionsWithoutCode.size(), [&](unsigned index) {
        willRelease(FunctionID(functionsWithoutCode[index]));
    });
    // Whether a thread finds the body of another function would depend on timing, so it never does: a body is hidden until it is compiled.
    m_state->hiddenBodies.grow(ids.size());
    forEachIndexInParallel(loops, ids.size(), [&](unsigned position) {
        willRelease(ids[position]);
        Typed::Function& function = program.at(ids[position]);
        if (function.bodyIsReleased)
            m_state->hiddenBodies[position] = Typed::ParkedBody(function.editAllBlocks(), function.editAllValues());
    });
    forEachIndexInParallel(loops, functionsWithoutCode.size(), [&](unsigned index) {
        m_state->release(FunctionID(functionsWithoutCode[index]));
    });
    m_state->phase.emplace(program.types, static_cast<unsigned>(m_state->batches.size()));
    m_state->caches.emplace(program, context);
}

CodeGenerationJob::~CodeGenerationJob()
{
    m_state->caches.reset();
    m_state->phase->discard();
}

GeneratedCode CodeGenerationJob::run()
{
    auto& [inputs, program, context, batches, ids, hiddenBodies, result, phase, caches] = *m_state;
    const ParallelLoops& loops = context.parallelLoops();
    // The records of all functions are hundreds of megabytes, and memory is at its highest here: a batch makes its own.
    result.functions.makeRoomFor(ids.size());
    std::span<CompiledCode> functions = result.functions.mutableSpan();
    Vector<CodeGenerationTotals> totals(context.threadCount());
    forEachIndexInParallel(loops, batches.size(), [&](unsigned index, unsigned worker) {
        Batch batch = batches[index];
        std::span<const FunctionID> idsInBatch = ids.subspan(batch.first, batch.size);
        std::span<CompiledCode> functionsInBatch = functions.subspan(batch.first, batch.size);
        for (uint32_t member = 0; member < batch.size; ++member) {
            result.functions.makeAt(batch.first + member);
            Typed::Function& function = program.at(idsInBatch[member]);
            if (!function.bodyIsReleased)
                continue;
            hiddenBodies[batch.first + member].takeInto(function.editAllBlocks(), function.editAllValues());
        }
        Typed::TypeArena arena(*phase, index);
        Typed::compileFunctions({ context, arena, context.backEndScratch(worker) }, inputs, idsInBatch, functionsInBatch, totals[worker]);
        for (uint32_t member = 0; member < batch.size; ++member) {
            auto& code = functionsInBatch[member];
            if (code.body.bytes.isEmpty())
                code.function = idsInBatch[member];
            else
                prepareForImage(code, context);
        }
        for (FunctionID id : idsInBatch)
            m_state->release(id);
    });

    for (auto& workerTotals : totals)
        result.entriesUsed.merge(workerTotals.entriesUsed);
    reportFunctionsWithoutCode(result.functions, context);
    return WTF::move(result);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
