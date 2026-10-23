/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFunctionCodeGenerator.h"

#if ENABLE(AOT)

#include "AOTASANShadowChecks.h"
#include "AOTBackEndReport.h"
#include "AOTCodeDescription.h"
#include "AOTCodeSizeMap.h"
#include "AOTElideStackChecks.h"
#include "AOTEntryEmitter.h"
#include "AOTFrameMaps.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionKinds.h"
#include "AOTHeat.h"
#include "AOTImageBuilder.h"
#include "AOTImageFormat.h"
#include "AOTImpliedBranchMerging.h"
#include "AOTIntegerUpdatesInMemory.h"
#include "AOTPhiNarrowing.h"
#include "AOTPrimitiveEmitter.h"
#include "AOTRepresentationRules.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTStubAssembler.h"
#include "AOTThrowEmitter.h"
#include "AOTVector.h"
#include "AirCode.h"
#include "AirLimits.h"
#include "B3Generate.h"
#include "CPU.h"
#include "StackAlignment.h"
#include <limits>
#include <wtf/DataLog.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/MakeString.h>

#ifndef AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE
#error "AOTRuntimeTable.h must define AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE."
#endif

namespace JSC { namespace AOT { namespace Typed {

static_assert(defaultLimitValue(LimitID::MaxFrameSizeInBytes, hostArchitecture()) == std::numeric_limits<decltype(ImageFrame::frameSizeInUnits)>::max() * stackAlignmentBytes());
static_assert(B3::AirLimits::maxFrameSizeWithPushedRegisters / sizeof(CPURegister) <= ImageFrame::maxCalleeSavesStart);
static_assert(B3::AirLimits::maxBytesBelowFrameRecord / sizeof(CPURegister) <= ImageFrame::maxCalleeSavesStart);

WTF_MAKE_TZONE_ALLOCATED_IMPL(FunctionCodeGenerator);

FunctionBackEndTraits computeBackEndTraits(const CodeGenerationInputs& inputs, FunctionID id, const Function& function, const FrameMaps& frameMaps)
{
    bool savesFrameOnSuspend = frameMaps.numberOfSuspensions() || function.role == FunctionRole::CoroutineBody;
    return {
        .function = findFunctionAsRead(&inputs.program, id),
        .convention = function.role == FunctionRole::StubBody ? BackEndConvention::StubBody : BackEndConvention::Function,
        .savesFrameOnSuspend = savesFrameOnSuspend,
        .readsCallerFrame = readsCallerFrame(inputs, id, function),
        .isAsync = function.has(FunctionFlags::IsAsync),
        .rejectsOnThrow = rejectsOnThrow(inputs.program, function),
        .hasHandlers = std::ranges::any_of(function.blocks, [](const Block& block) { return !!block.handler; }),
        .isCold = isCold(inputs.heat.of(inputs.program, id)),
        .runsOnce = inputs.heat.of(inputs.program, id) == HeatClass::RunsOnce,
        .isHot = isHot(inputs.heat.of(inputs.program, id)),
        .returnsAreUnlikely = !savesFrameOnSuspend && !function.hasAny({ FunctionFlags::IsAsync, FunctionFlags::IsGenerator }),
    };
}

static BitVector findAirBlocksReachedWithoutRareEdge(const B3::Air::Code& code)
{
    BitVector reachedBlocks(code.size());
    Vector<B3::Air::BasicBlock*, 16> worklist;
    auto follow = [&](const B3::Air::FrequentedBlock& edge) {
        if (!edge.isRare() && !reachedBlocks.quickSet(edge.block()->index()))
            worklist.append(edge.block());
    };
    for (const B3::Air::FrequentedBlock& entrypoint : code.entrypoints())
        follow(entrypoint);
    while (!worklist.isEmpty()) {
        for (const B3::Air::FrequentedBlock& successor : worklist.takeLast()->successors())
            follow(successor);
    }
    return reachedBlocks;
}

// A loop behind a rare edge is entered rarely and then runs as often as any other loop, so it is not rare code.
static BitVector findRareAirBlocks(const B3::Air::Code& code)
{
    BitVector rareBlocks(code.size());
    BitVector reachedBlocks = findAirBlocksReachedWithoutRareEdge(code);
    auto forEachLikelySuccessorAmongRareBlocks = [&](B3::Air::BasicBlock* block, const auto& functor) {
        for (const B3::Air::FrequentedBlock& successor : block->successors()) {
            if (!successor.isRare() && !reachedBlocks.quickGet(successor.block()->index()))
                functor(successor.block()->index());
        }
    };

    // Every cycle has an edge that does not lead to a later block.
    bool mayHaveCycle = false;
    for (B3::Air::BasicBlock* block : code) {
        if (reachedBlocks.quickGet(block->index()))
            continue;
        rareBlocks.quickSet(block->index());
        forEachLikelySuccessorAmongRareBlocks(block, [&](unsigned successorIndex) {
            mayHaveCycle |= successorIndex <= block->index();
        });
    }
    if (!mayHaveCycle)
        return rareBlocks;

    Vector<Vector<uint32_t, 2>> likelySuccessors(code.size());
    for (size_t blockIndex : rareBlocks) {
        forEachLikelySuccessorAmongRareBlocks(code[blockIndex], [&](unsigned successorIndex) {
            likelySuccessors[blockIndex].append(successorIndex);
        });
    }
    StronglyConnectedComponents components = computeStronglyConnectedComponents(code.size(), [&](unsigned block) -> const Vector<uint32_t, 2>& {
        return likelySuccessors[block];
    });
    rareBlocks.exclude(components.isInCycle);
    return rareBlocks;
}

BlockColdnessRule::BlockColdnessRule(const CodeGenerationInputs& inputs, FunctionID id)
    : m_inputs(inputs)
    , m_id(id)
    , m_activations(inputs.heat.activationsOf(inputs.program, id))
    // Nothing says that a compact function runs, and a failed test at an entrance is expected of no call.
    , m_isExpectedNotToBeEntered(inputs.heat.isCompact(inputs.program, id) || inputs.program.at(id).runsOnlyBehindFailedTest())
{
}

BlockColdness BlockColdnessRule::of(double frequency, bool isRare) const
{
    double heat = m_activations * frequency;
    HeatClass heatClass = m_inputs.heat.classOf(m_inputs.program, m_id, frequency);
    // A shared sequence costs a call and a return each time it runs.
    bool isExpectedNotToRun = isCold(heatClass) && !m_inputs.heat.isColdByLimitAlone(heat);
    return {
        .isCold = isExpectedNotToRun || (isRare && !isHot(heatClass)),
        // In a function that is expected to run, what lies behind a rare edge is rare by an estimate only: a test that rests on a guess fails at every call or at none.
        .isRarelyRun = (isRare && isExpectedNotToRun) || heatClass == HeatClass::RunsOnce || m_isExpectedNotToBeEntered,
    };
}

ColdAirBlocks findColdAirBlocks(const CodeGenerationInputs& inputs, FunctionID id, const B3::Air::Code& code)
{
    BlockColdnessRule rule(inputs, id);
    BitVector rareBlocks = findRareAirBlocks(code);
    ColdAirBlocks result { BitVector(code.size()), BitVector(code.size()) };
    for (B3::Air::BasicBlock* block : code) {
        BlockColdness coldness = rule.of(block->frequency(), rareBlocks.quickGet(block->index()));
        if (coldness.isCold)
            result.cold.quickSet(block->index());
        if (coldness.isRarelyRun)
            result.rarelyRun.quickSet(block->index());
    }
    return result;
}

static BitVector findNarrowReferences(const Function& function)
{
    BitVector references;
    for (uint32_t index = 0; index < function.values.size(); ++index) {
        if (function.values[index].representation == Representation::Cell32)
            references.set(index);
    }
    return references;
}

static BitVector findParametersWithoutObjects(const Program& program, const Function& function)
{
    BitVector parameters;
    for (uint32_t index = 0; index < function.values.size(); ++index) {
        if (function.values[index].opcode != Opcode::Parameter)
            continue;
        const Type& type = program.typeOf(function, ValueID(index));
        if (type.isJavaScriptValue() && !type.hasObjects())
            parameters.set(index);
    }
    return parameters;
}

static void appendClientPhases(B3::Policy& policy, const Program& program, const Function& function, const FunctionBackEndTraits& traits)
{
    policy.clientPhases.append(mergeImpliedBranchesAfterOptimization(program, function));
    policy.clientPhases.append(narrowTruncatedPhisAfterOptimization());
    policy.clientPhases.append(updateIntegersInMemoryAfterOptimization());
    if (traits.savesFrameOnSuspend)
        policy.clientPhases.append(materializeSlotBasesLate());
#if AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE
    policy.clientPhases.append(checkASANShadowBeforeAccesses());
#endif
}

std::unique_ptr<FunctionCodeGenerator> FunctionCodeGenerator::create(const CodeGenerationWorker& worker, const CodeGenerationInputs& inputs, FunctionID id, CodeGenerationTotals& totals, DirectCodeAttempt attempt)
{
    std::unique_ptr<FunctionCodeGenerator> generator(new FunctionCodeGenerator(worker, inputs, id, totals));
    if (!generator->generateB3(attempt))
        return nullptr;
    return generator;
}

FunctionCodeGenerator::FunctionCodeGenerator(const CodeGenerationWorker& worker, const CodeGenerationInputs& inputs, FunctionID id, CodeGenerationTotals& totals)
    : m_worker(worker)
    , m_inputs(inputs)
    , m_id(id)
    , m_function(inputs.program.at(id))
    , m_relocations(m_jit, worker.build.assemblerPolicy())
    , m_relocationLowering(ImageRelocationLowering::instanceRuntimeTable(), m_relocations)
    , m_records(inputs.stubCatalog, m_relocations, totals)
{
}

bool FunctionCodeGenerator::generateB3(DirectCodeAttempt attempt)
{
    BuildContext& build = m_worker.build;
    const AOT::CompileOptions& options = build.options();
    const Program& program = m_inputs.program;

    AnalysisContext context { build, program, &m_inputs.flow, &m_inputs.effects, m_id };
    FunctionAnalyses analyses(context, m_function);
    FrameMaps frameMaps = computeFrameMaps(program, m_function, analyses);

    FunctionBackEndTraits traits = computeBackEndTraits(m_inputs, m_id, m_function, frameMaps);
    B3::Policy policy = b3PolicyFor(build, traits, m_relocationLowering);
    policy.clientPhases.append(throwWhereTrapsAreCertain(m_records, m_inputs.stubCatalog));
    appendClientPhases(policy, program, m_function, traits);
    if (traits.savesFrameOnSuspend && options.verifiesProgram)
        policy.clientPhases.append(validateFrameAddresses(m_frameAddressViolations, frameMaps.suspensionValues(), findParametersWithoutObjects(program, m_function)));
    if (!options.recorded.useWideReferences() && !options.recorded.has(TestingSwitch::DropReferenceGuards)) {
        NarrowReferenceOrigins origins { findNarrowReferences(m_function), frameMaps.suspensionValues(), &m_records.operationsWithCompressedOperand };
        if (std::optional<DroppedGuard> dropped = droppedGuardIn(options.recorded, m_id)) {
            origins.ordinalOfDroppedGuard = dropped->ordinal;
            for (uint32_t index = 0; index < m_function.values.size(); ++index) {
                if (m_function.values[index].opcode == dropped->opcode)
                    origins.originsOfDroppedGuard.set(index);
            }
        }
        policy.clientPhases.append(guardNarrowReferences(m_numberOfLateReferenceGuards, WTF::move(origins)));
    }
    if (options.validatesReferences && !options.recorded.useWideReferences())
        policy.clientPhases.append(validateNarrowReferences(m_narrowReferenceFindings, { findNarrowReferences(m_function), frameMaps.suspensionValues(), &m_records.operationsWithCompressedOperand }));
#if AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE
    m_records.totals.entriesUsed.set(static_cast<size_t>(AOT::Entry::operationAOTReportPoisonedAccess));
#endif
    m_records.stubCalls.setCaller(m_id);
    m_records.stubCalls.setChecksPreservedRegisters(options.validatesBackEnd);
    if (build.validates())
        m_records.totals.entriesUsed.set(static_cast<size_t>(AOT::Entry::operationAOTReportValidationFailure));

    m_procedure = makeUnique<B3::Procedure>(policy, &m_worker.scratch);
    B3::Procedure& proc = *m_procedure;
    pinRegisters(proc);
    if (build.shouldDump(m_id))
        proc.setName(makeString('#', m_id.index(), '#'));
    if (options.writesCodeSizeMap()) [[unlikely]]
        proc.setNeedsPCToOriginMap();
    setStackCheckGenerator(proc, m_inputs.stackChecks.of(m_id), m_records.frame, m_stackChecks);

    m_records.frame.readsCallerFrame = traits.readsCallerFrame;
    CodeGenerator generator(FunctionInputs { build, m_worker.types, m_inputs, m_id, m_function, analyses, frameMaps }, proc, m_records);
    if (generator.run() == GenerationResult::Failed)
        return false;
    RELEASE_ASSERT(m_records.frame.savesFrameOnSuspend <= traits.savesFrameOnSuspend);
    RELEASE_ASSERT(m_records.frame.readsCallerFrame == traits.readsCallerFrame);

    if (options.logging.dumpsB3Graph && build.shouldDump(m_id)) [[unlikely]] {
        dataLogLn("AOT B3:\n", proc);
        proc.setShouldDumpIR();
    }

    bool isWorthOptimizing = !isGeneratedWithoutOptimizing(build, m_inputs.heat, m_inputs.program, m_id);
    bool checksAir = options.validatesBackEnd || options.validatesReferences;
    if (!isWorthOptimizing && !checksAir && attempt != DirectCodeAttempt::None && traits.convention == BackEndConvention::Function)
        m_directCode = planDirectCode(proc, attempt);
    return true;
}

CompiledCode FunctionCodeGenerator::finish()
{
    CompiledCode code;
    if (!m_procedure->report().failure)
        code = emitCode();
    if (m_didFailToGenerateDirectly) {
        m_worker.build.statistics().count(StatisticID::DirectGenerationsAbandoned);
        return { };
    }
    if (generatesDirectly())
        m_worker.build.statistics().count(StatisticID::FunctionsGeneratedDirectly);
    if (reportBackEnd())
        return { };
    return code;
}

CompiledCode FunctionCodeGenerator::emitCode()
{
    BuildContext& build = m_worker.build;
    const AOT::CompileOptions& options = build.options();
    B3::Procedure& proc = *m_procedure;

    if (!generatesDirectly()) {
        ColdAirBlocks coldBlocks = findColdAirBlocks(m_inputs, m_id, proc.code());
        m_records.units.setColdBlocks(WTF::move(coldBlocks.cold), WTF::move(coldBlocks.rarelyRun));
        proc.code().setInstObserver(&m_records.observer);
    }

    readTagsFromInstance(m_jit);
    emitEntryPreambles(m_jit, m_inputs, m_id, m_records);
    CCallHelpers::Label bodyStart = m_jit.label();
    if (generatesDirectly()) {
        BlockColdnessRule coldness(m_inputs, m_id);
        m_records.frameMaps.anotherGeneratorCanTry = true;
        bool isGenerated = generateDirectCode(*m_directCode, m_jit, m_records, coldness);
        if (!isGenerated || m_records.frameMaps.hasSiteWithoutWord) {
            m_didFailToGenerateDirectly = true;
            return { };
        }
    } else
        B3::generate(proc, m_jit);
    RELEASE_ASSERT(!CCallHelpers::differenceBetween(bodyStart, proc.code().entrypointLabel(0)));
    bool preamblesHavePrimaryEntry = m_records.entries.containsIf([](const RecordedEntry& entry) {
        return entry.id == primaryEntry;
    });
    if (!preamblesHavePrimaryEntry)
        m_records.entries.insert(0, RecordedEntry { primaryEntry, proc.code().entrypointLabel(0) });
    emitStackOverflowPaths(m_jit, m_records, m_stackChecks);
    CCallHelpers::Label paddingStart = m_jit.label();
    m_jit.breakpoint();
    m_jit.padBeforePatch();

    ImageLinkBuffer buffer(m_jit, LinkBuffer::Profile::FTL);
    if (buffer.didFailToAllocate()) {
        reportLinkFailure(LinkFailure::OutOfMemory);
        return { };
    }
    if (!frameFitsImage())
        return { };

    buffer.finalize(options.logging.dumpsDisassembly && build.shouldDump(m_id), "AOT code"_s);
    if (proc.report().failure)
        return { };

    auto relocations = m_relocations.link(buffer, &build);
    if (!relocations) {
        reportLinkFailure(relocations.error());
        return { };
    }

    CompiledCode code = describeCode(build, m_inputs, m_id, m_function, m_records, EmittedCode { buffer, &proc, m_stackChecks, paddingStart, WTF::move(*relocations) });
    if (!code.frame.hasFrame())
        build.statistics().count(StatisticID::FunctionsWithoutFrame);
    if (proc.code().frameLayout().coverage == B3::Air::FrameCoverage::SomePaths)
        build.statistics().count(StatisticID::FunctionsWithFrameOnSomePaths);
    if (code.frame.isEnteredWithoutStackCheck)
        build.statistics().count(StatisticID::FunctionsEnteredWithoutStackCheck);
    if (options.writesCodeSizeMap()) [[unlikely]]
        code.sizesByOperation = makeString(describeCodeSize(m_inputs.program, m_function, proc.pcToOriginMap(), buffer), describeBranches(m_inputs, m_id, m_function, build.recordedOptions()));
    return code;
}

bool FunctionCodeGenerator::frameFitsImage()
{
    BuildContext& build = m_worker.build;
    unsigned frameSize = m_procedure->frameSize();
    RegisterAtOffsetList calleeSaves = m_procedure->calleeSaveRegisterAtOffsetList();
    RELEASE_ASSERT(!calleeSaves.registerCount() || -calleeSaves.at(0).offset() / static_cast<ptrdiff_t>(sizeof(CPURegister)) <= static_cast<ptrdiff_t>(ImageFrame::maxCalleeSavesStart));
    if (!build.exceeds(LimitID::MaxFrameSizeInBytes, frameSize, { .function = m_id }))
        return true;
    build.diagnostics().add(UserDiagnostic { .kind = UserDiagnosticKind::FrameTooLarge, .function = m_id, .limit = LimitID::MaxFrameSizeInBytes, .count = frameSize });
    return false;
}

void FunctionCodeGenerator::reportLinkFailure(LinkFailure failure)
{
    AOT::reportLinkFailure(failure, m_id, m_worker.build);
}

bool FunctionCodeGenerator::reportBackEnd()
{
    BuildContext& build = m_worker.build;
    const B3::CompilationReport& report = m_procedure->report();
    BackEndReport collected = collectBackEndReport(report, m_frameAddressViolations.span(), build.options().collectsDeclines());
    // Every narrow reference must be in a form that the scan of the stack finds. That a pointer lies beside it is asked for by a check of its own.
    if (!build.options().validatesReferenceGuards) {
        m_narrowReferenceFindings.violations.removeAllMatching([](const NarrowReferenceViolation& violation) {
            return violation.kind == NarrowReferenceViolationKind::HasNoPointer;
        });
    }
    appendFindings(collected, m_narrowReferenceFindings.violations.span());
    build.statistics().count(StatisticID::NarrowReferencesAcrossSafepoints, m_narrowReferenceFindings.numberOfReferencesAcrossSafepoints);
    build.statistics().count(StatisticID::NarrowReferenceGuards, m_numberOfLateReferenceGuards);
    build.statistics().count(StatisticID::NarrowReferencesStoredInFrames, m_narrowReferenceFindings.numberOfReferencesStoredInFrame);
    return AOT::reportBackEnd(build, m_id, m_function, collected, report);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
