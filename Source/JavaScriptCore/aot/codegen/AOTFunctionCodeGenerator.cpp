/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFunctionCodeGenerator.h"

#if ENABLE(AOT)

#include "AOTCodeDescription.h"
#include "AOTCurrentWork.h"
#include "AOTEntryEmitter.h"
#include "AOTFunctionKinds.h"
#include "AOTIRProgram.h"
#include "AOTImageBuilder.h"
#include "AOTImageFormat.h"
#include "AOTMachineAllocation.h"
#include "AOTMachineFrame.h"
#include "AOTMachineOrder.h"
#include "AOTMachinePrinter.h"
#include "AOTMachineBuilder.h"
#include "AOTMachineChecker.h"
#include "AOTStraightEmitter.h"
#include "AOTStubAssembler.h"
#include "CPU.h"
#include "StackAlignment.h"
#include <limits>
#include <wtf/StringPrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(defaultLimitValue(LimitID::MaxFrameSizeInBytes, hostArchitecture()) == std::numeric_limits<decltype(ImageFrame::frameSizeInUnits)>::max() * stackAlignmentBytes());

static bool runsMachineChecks(const BuildContext& build)
{
#if ASSERT_ENABLED || ASAN_ENABLED
    UNUSED_PARAM(build);
    return true;
#else
    return build.options().verifiesMachineCode;
#endif
}

Machine::ProcedureTraits machineTraitsOf(const BuildContext& build, const CodeGenerationInputs& inputs, const Code& code)
{
    CodeUnit unit = code.unit();
    const CallingConventions& conventions = inputs.plan.conventions();
    bool isBodyThatMaySuspend = isBodyOfCoroutine(unit.role, conventions.isCoroutine(unit.function)) && !conventions.of(unit.function).coroutineNeverSuspends;
    bool savesFrameOnSuspend = code.hasSuspensions() || isBodyThatMaySuspend;
    const Function& function = inputs.program.at(unit.function);
    bool isBodyOfAsyncFunction = unit.role == CodeRole::CoroutineBody && function.has(FunctionFlags::IsAsync) && !function.has(FunctionFlags::IsGenerator);
    RELEASE_ASSERT(!savesFrameOnSuspend || !conventions.spillsArgumentRegisters(unit));
    RELEASE_ASSERT(!savesFrameOnSuspend || !conventions.savesRegistersOfCallers(unit));
    return {
        .savesFrameOnSuspend = savesFrameOnSuspend,
        .hasHandlers = code.hasHandlers(),
        .rejectsOnThrow = rejectsOnThrow(inputs.program, conventions, unit),
        .savesRegistersOfCallers = conventions.savesRegistersOfCallers(unit),
        .usesNarrowReferences = !inputs.plan.usesWideReferences(),
        .fallsIntoWhatLiesBehind = unit.role == CodeRole::ParameterGuard,
        .chargesForCalleeSaves = isX86_64(),
        .mayOmitFrameRecord = isX86_64(),
        .mayJumpOverUnlikelySides = isBodyOfAsyncFunction || function.isModuleBody(),
        .checksAllocation = runsMachineChecks(build),
        .shouldUseFallbackBlockOrder = hasFlag(build.options().testing, Wire::TestingSwitch::ForceFallbackBlockOrder),
        .shouldDropPatchOnlySaves = hasFlag(build.options().testing, Wire::TestingSwitch::DropPatchOnlySaves),
        .shouldAssumeSpillSlotsDead = hasFlag(build.options().testing, Wire::TestingSwitch::AssumeSpillSlotsDead),
        .shouldAssumeClearUpperHalves = hasFlag(build.options().injectedFaults, Wire::FaultInjection::AssumeClearUpperHalves),
        .shouldIgnoreCollectorSpillReads = hasFlag(build.options().injectedFaults, Wire::FaultInjection::IgnoreCollectorSpillReads),
        .notesRangesOfOrigins = hasFlag(build.options().reports, ReportSet::Flow) || !build.options().logging.mapFilePath.isNull(),
        .features = build.targetFeatures(),
    };
}

static void enterStepOfCodeGeneration(ASCIILiteral step)
{
    currentWorkOfThread().step.store(step.characters(), std::memory_order_relaxed);
}

static bool someOperationGivenToEmittersMayCollect(const Facts& facts, const Code& code)
{
    FunctionID function = code.function();
    if (!facts.effectsOnReturn(function).mayCollect && !facts.effectsOnThrow(function).mayCollect)
        return false;
    uint32_t numberOfBlocks = code.numberOfBlocks();
    for (uint32_t index = 0; index < numberOfBlocks; ++index) {
        if (!code.isReached(BlockID(index)))
            continue;
        for (ValueID value : code.valuesOf(BlockID(index))) {
            if (code.at(value).traits().mayCollect())
                return true;
        }
    }
    return false;
}

static uint64_t countMachineOperationsInOrder(const Machine::Order& order)
{
    uint64_t count = 0;
    for (const Machine::Block* block : order.blocksInOrder())
        count += block->phis().size() + block->size() + block->upsilons().size();
    return count;
}

static InternalErrorKind internalErrorKindOfMachineFailure(Machine::FailureKind kind)
{
    switch (kind) {
    case Machine::FailureKind::MalformedProcedure:
        return InternalErrorKind::MalformedMachineProcedure;
    case Machine::FailureKind::OutOfRegisters:
        return InternalErrorKind::OutOfRegisters;
    case Machine::FailureKind::AccessOutsideDeclaredEffects:
        return InternalErrorKind::StoreOutsideDeclaredEffects;
    case Machine::FailureKind::WrongValueInRegister:
        return InternalErrorKind::WrongValueInRegister;
    case Machine::FailureKind::WrongValueInSpillSlot:
        return InternalErrorKind::WrongValueInSpillSlot;
    case Machine::FailureKind::FrameAddressAcrossSuspension:
        return InternalErrorKind::FrameAddressAcrossSuspension;
    case Machine::FailureKind::NarrowReferenceWithoutPointer:
        return InternalErrorKind::NarrowReferenceWithoutPointer;
    case Machine::FailureKind::InnerPointerWithoutOwner:
        return InternalErrorKind::InnerPointerWithoutOwner;
    case Machine::FailureKind::MissingFrameSetup:
        return InternalErrorKind::MissingFrameSetup;
    case Machine::FailureKind::UpperHalfIsNotClear:
        return InternalErrorKind::UpperHalfNotClear;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void reportFailureOfMachineProcedure(BuildContext& build, const Program& program, const Code& code, const Machine::Procedure& procedure)
{
    const Machine::Failure& failure = *procedure.failure();

    StringPrintStream text;
    Machine::printFailure(text, procedure);

    InternalError error {
        .kind = internalErrorKindOfMachineFailure(failure.kind),
        .function = code.function(),
        .location = program.at(code.function()).declarationLocation(),
        .detail = text.toString(),
    };
    ValueID value = valueOfOrigin(failure.origin);
    if (value && value.index() < code.numberOfValues()) {
        const Operation& operation = code.at(value);
        Wire::SourceLocation location = code.locationOf(program, operation.origin());
        if (hasModule(location))
            error.location = location;
        if (hasModule(error.location)) {
            error.block = operation.block();
            error.value = value;
            error.operation = nameOf(operation.opcode);
        }
    }
    build.diagnostics().add(error);
}

FunctionCodeGenerator::FunctionCodeGenerator(const CodeGenerationWorker& worker, const CodeGenerationInputs& inputs, const Code& code, CodeGenerationTotals& totals)
    : m_worker(worker)
    , m_inputs(inputs)
    , m_code(code)
    , m_statistics(worker.build.statistics().countersOfCurrentThread())
    , m_relocations(m_jit, worker.build.assemblerPolicy())
    , m_records(inputs.stubCatalog, m_relocations, totals, &worker.arena.objectKeptAcrossResets<RememberedStubKeys>())
    , m_procedure(worker.arena, inputs.heaps.tree(), machineTraitsOf(worker.build, inputs, code))
{
    if (hasFlag(worker.build.options().reports, ReportSet::InlinedStrings))
        m_records.inlinedStrings.beginRecording();
}

CompiledCode FunctionCodeGenerator::run()
{
    if (!appendOperations())
        return { };
    return emitCode();
}

bool FunctionCodeGenerator::appendOperations()
{
    BuildContext& build = m_worker.build;

    StackCheck stackCheck = m_code.holdsCallThatPlanHasNotCounted() ? StackCheck::Required : m_inputs.plan.conventions().stackCheckOf(m_code.unit());
    setStackCheckGenerator(m_procedure, stackCheck, m_records.frame, m_stackChecks);
    m_stackChecks.functionMayCollect = someOperationGivenToEmittersMayCollect(m_inputs.facts, m_code);

    bool unitReadsCallerFrame = readsCallerFrame(m_inputs, m_code);
    m_records.frame.readsCallerFrame = unitReadsCallerFrame;

    CodeGenerator generator(FunctionInputs { build, m_worker.types, m_inputs, m_code }, m_procedure, m_records);
    if (generator.run() == GenerationResult::Failed)
        return false;
    if (m_procedure.failure() || (generator.keepsValuesInHeapFrame() && !runsMachineChecks(build) && !Machine::checkThatDefinitionsDominateUses(m_procedure))) {
        reportFailureOfMachineProcedure(m_worker.build, m_inputs.program, m_code, m_procedure);
        return false;
    }
    RELEASE_ASSERT(m_records.frame.savesFrameOnSuspend <= m_procedure.traits().savesFrameOnSuspend);
    RELEASE_ASSERT(m_records.frame.readsCallerFrame == unitReadsCallerFrame);

    m_statistics.count(StatisticID::OperationsEmitted, m_code.numberOfOperationsInOrder());
    return true;
}

void FunctionCodeGenerator::appendStraightBody(StraightEmitter& emitter)
{
    CodeUnit unit = m_code.unit();
    setStackCheckGenerator(m_procedure, emitter.entersUnits() ? StackCheck::Required : m_inputs.plan.conventions().stackCheckOf(unit), m_records.frame, m_stackChecks);
    m_stackChecks.functionMayCollect = m_inputs.facts.effectsOnReturn(unit.function).mayCollect || m_inputs.facts.effectsOnThrow(unit.function).mayCollect;
    m_records.frame.readsCallerFrame = readsCallerFrame(m_inputs, m_code);
    m_records.sites.inlineCallSites.append(0);

    Machine::Builder builder(m_procedure);
    builder.appendTo(m_procedure.addEntrypoint(primaryEntry));
    Machine::StackSlot* slots = m_procedure.addStackSlot(std::max<unsigned>((emitter.numberOfSlots() + 1) & ~1u, 2) * sizeof(EncodedJSValue));
    m_procedure.requestCallArgAreaSizeInBytes(WTF::roundUpToMultipleOf<stackAlignmentBytes()>(emitter.wordsOfScratchArea() * sizeof(EncodedJSValue)));

    const ArgumentLocations* locations = m_worker.arena.create<ArgumentLocations>(m_inputs.plan.conventions().locationsOf(m_inputs.program, unit));
    Vector<Machine::ConstrainedValue, 8> passed;
    for (const ArgumentLocation& location : locations->parameters) {
        if (location.kind == ArgumentLocation::Kind::GPR || location.kind == ArgumentLocation::Kind::FPR)
            passed.append(Machine::ConstrainedValue(builder.argumentRegister(location.reg), Machine::ValueRep::reg(location.reg)));
    }
    if (AOT::hasFlag(m_worker.build.options().testing, Wire::TestingSwitch::CheckEffectsAtRunTime)) [[unlikely]]
        countEntryIntoCode(builder.patch(Machine::Void));
    Machine::PatchOperation* patch = builder.patch(Machine::Void);
    for (const Machine::ConstrainedValue& value : passed)
        patch->append(value);
    patch->effects.terminal = true;
    patch->setNeedsFrameRecord(true);
    RegisterSet clobbered = registersClobberedByCompiledCalls();
    clobbered.merge(RegisterSet::macroClobberedGPRs());
    patch->clobberLate(clobbered);
    patch->setGenerator([emitter = &emitter, slots, locations](CCallHelpers& jit, const Machine::PatchParameters& params) {
        RELEASE_ASSERT(params.frameIsSetUp() && slots->offsetFromFP() == -static_cast<intptr_t>(slots->byteSize()));
        emitter->generate(jit, *locations, params.frame().bytesBelowFrameRecord());
    });
}

std::optional<CompiledCode> FunctionCodeGenerator::runStraight(const BodyIndex& index)
{
    StraightEmitter emitter(m_worker.build, m_worker.types, m_inputs, m_code.unit(), index, m_records, m_worker.arena);
    appendStraightBody(emitter);
    RELEASE_ASSERT(!m_procedure.failure());
    m_straightEmitter = &emitter;
    CompiledCode code = emitCode();
    if (emitter.hasFailed())
        return std::nullopt;
    if (uint32_t reads = emitter.numberOfRawSlotsOfScopesRead())
        m_worker.build.statistics().count(StatisticID::StraightUnboxedSlotReads, reads);
    if (uint32_t told = emitter.numberOfNumbersToldByTheirWord())
        m_worker.build.statistics().count(StatisticID::StraightNumberTagChecks, told);
    if (uint32_t reads = emitter.numberOfReadsThatAskTheirSite())
        m_worker.build.statistics().count(StatisticID::StraightSiteCachedReads, reads);
    return code;
}

CompiledCode FunctionCodeGenerator::runEntryWrittenByHand(std::span<const QuestionOfEntry> questions, bool nobodyEnters)
{
    CodeUnit unit = m_code.unit();
    setStackCheckGenerator(m_procedure, StackCheck::CoveredByCallers, m_records.frame, m_stackChecks);
    m_records.sites.inlineCallSites.append(0);

    Machine::Builder builder(m_procedure);
    builder.appendTo(m_procedure.addEntrypoint(primaryEntry));
    ArgumentLocations locations = m_inputs.plan.conventions().locationsOf(m_inputs.program, unit);
    Vector<Machine::ConstrainedValue, 8> passed;
    for (const ArgumentLocation& location : locations.parameters) {
        if (location.kind == ArgumentLocation::Kind::GPR || location.kind == ArgumentLocation::Kind::FPR)
            passed.append(Machine::ConstrainedValue(builder.argumentRegister(location.reg), Machine::ValueRep::reg(location.reg)));
    }
    if (AOT::hasFlag(m_worker.build.options().testing, Wire::TestingSwitch::CheckEffectsAtRunTime)) [[unlikely]]
        countEntryIntoCode(builder.patch(Machine::Void));
    Machine::PatchOperation* patch = builder.patch(Machine::Void);
    for (const Machine::ConstrainedValue& value : passed)
        patch->append(value);
    patch->effects.terminal = true;
    patch->setRunsWithoutFrame(true);
    RegisterSet clobbered = registersClobberedByCompiledCalls();
    clobbered.merge(RegisterSet::macroClobberedGPRs());
    patch->clobberLate(clobbered);
    patch->setGenerator([this, unit, questions, nobodyEnters](CCallHelpers& jit, const Machine::PatchParameters& params) {
        emitEntryWrittenByHand(jit, m_inputs, m_records, unit, questions, params.frameIsSetUp(), nobodyEnters);
    });
    RELEASE_ASSERT(!m_procedure.failure());
    return emitCode();
}

CompiledCode FunctionCodeGenerator::emitCode()
{
    BuildContext& build = m_worker.build;

    enterStepOfCodeGeneration("allocate"_s);
    if (m_stackChecks.stackCheck == StackCheck::CoveredByCallers && !m_records.frame.needsStackCheckForTailCalls)
        m_procedure.setLargestFrameSetUpWithoutStackCheck(maxFrameSizeWithoutStackCheck);
    Machine::prepareForGeneration(m_procedure);
    if (m_procedure.failure()) {
        reportFailureOfMachineProcedure(m_worker.build, m_inputs.program, m_code, m_procedure);
        return { };
    }
    if (!frameFitsImage())
        return { };

    const Machine::Allocation& allocation = m_procedure.allocation();
    m_statistics.count(StatisticID::MachineOperationsAppended, m_procedure.numberOfOperations());
    m_statistics.count(StatisticID::MachineOperationsScanned, countMachineOperationsInOrder(m_procedure.order()));
    m_statistics.count(StatisticID::LoopsRotated, m_procedure.order().numberOfLoopsRotated());
    m_statistics.count(StatisticID::NumbersChosenWithoutBranches, m_procedure.numberOfNumbersChosen());
    m_statistics.count(StatisticID::MachineOperationsAllocated, allocation.numberOfOperationsAllocated());
    m_statistics.count(StatisticID::ValuesSpilled, allocation.spills().size());
    m_statistics.count(StatisticID::SpilledFirstOperandsRead, allocation.numberOfSpilledFirstOperands());
    m_statistics.count(StatisticID::LaterUsesScannedForLiveness, allocation.laterUsesScanned());
    m_statistics.count(StatisticID::BlocksVisitedByLivenessSearches, allocation.livenessSearchBlocksVisited());
    m_statistics.count(StatisticID::InnerPointersAcrossSafepoints, m_procedure.numberOfInnerPointersAcrossSafepoints());

    enterStepOfCodeGeneration("encode"_s);
    readTagsFromInstance(m_jit);
    CCallHelpers::Jump fromFusedCallEntry = emitEntryPreambles(m_jit, m_inputs, m_code.unit(), m_records);
    CCallHelpers::Label bodyStart = m_jit.label();
    m_relocations.reserveRoomFor(allocation.numberOfOperationsAllocated());
    Machine::generate(m_procedure, m_jit, m_relocations);
    m_statistics.count(StatisticID::LoopsEnteredByTest, m_procedure.numberOfLoopsEnteredByTest());
    if (m_straightEmitter && m_straightEmitter->hasFailed())
        return { };
    if (m_procedure.failure()) {
        reportFailureOfMachineProcedure(m_worker.build, m_inputs.program, m_code, m_procedure);
        return { };
    }
    const CallingConventions& conventions = m_inputs.plan.conventions();
    bool holdsFusedCallEntry = conventions.holdsFusedCallEntry(m_code.unit());
    bool isGuard = m_code.unit().role == CodeRole::ParameterGuard;
    bool preambleFallsThrough = m_records.frame.spillsArgumentRegisters || (holdsFusedCallEntry && !isGuard);
    RELEASE_ASSERT(!preambleFallsThrough || !CCallHelpers::differenceBetween(bodyStart, m_procedure.entrypointLabel(0)));
    if (fromFusedCallEntry.isSet())
        fromFusedCallEntry.linkTo(m_procedure.entrypointLabel(0), &m_jit);
    else if (holdsFusedCallEntry && isGuard)
        m_records.entries.append(RecordedEntry { fusedCallEntry, m_procedure.entrypointLabel(0) });
    m_statistics.count(StatisticID::MachineOperationsEncoded, m_procedure.numberOfOperationsEncoded());
    m_statistics.count(StatisticID::SwitchBitTestsEmitted, m_procedure.switchBitTestCount());
    m_statistics.count(StatisticID::NullChecksFoldedIntoAccesses, m_procedure.numberOfTrapsTakenOver());

    if (!m_stackChecks.pending.isEmpty())
        emitStackOverflowPaths(m_jit, m_records, m_stackChecks);
    m_records.stubCalls.routeTransfersThroughEnd();
    emitPadding(m_jit);
    m_jit.padBeforePatch();

    ImageLinkBuffer buffer(m_jit, LinkBuffer::Profile::FTL);
    if (buffer.didFailToAllocate()) {
        reportLinkFailure(LinkFailure::OutOfMemory);
        return { };
    }
    buffer.finalize(false, "AOT code"_s);

    auto relocations = m_relocations.link(buffer, &build);
    if (!relocations) {
        reportLinkFailure(relocations.error());
        return { };
    }

    CompiledCode compiled = describeCode(build, m_inputs, m_code, m_records, EmittedCode { buffer, m_procedure, m_stackChecks, WTF::move(*relocations) });
    if (m_worker.types.hasMadeTexts())
        copyMadeTextsMentionedBy(compiled.body, m_worker.types);
    return compiled;
}

bool FunctionCodeGenerator::frameFitsImage()
{
    BuildContext& build = m_worker.build;
    const Machine::Frame& frame = m_procedure.frame();
    const RegisterAtOffsetList& calleeSaves = frame.calleeSaveRegisterAtOffsetList();
    RELEASE_ASSERT(!calleeSaves.registerCount() || -calleeSaves.at(0).offset() / static_cast<ptrdiff_t>(sizeof(CPURegister)) <= static_cast<ptrdiff_t>(ImageFrame::maxCalleeSavesStart));

    unsigned frameSize = frame.frameSize();
    RELEASE_ASSERT(!(frameSize % stackAlignmentBytes()));
    if (frameSize <= build.limit(LimitID::MaxFrameSizeInBytes))
        return true;
    build.diagnostics().add(UserDiagnostic { .kind = UserDiagnosticKind::FrameTooLarge, .function = m_code.function(), .limit = LimitID::MaxFrameSizeInBytes, .count = frameSize });
    return false;
}

void FunctionCodeGenerator::reportLinkFailure(LinkFailure failure)
{
    AOT::reportLinkFailure(failure, m_code.function(), m_worker.build);
}

CompiledCode generateCode(const CodeGenerationWorker& worker, const CodeGenerationInputs& inputs, const Code& code, CodeGenerationTotals& totals)
{
    worker.arena.reset();
    RELEASE_ASSERT(code.isSwept());

    CurrentWorkScope work(inputs.program, code.function(), "emit"_s);
    FunctionCodeGenerator generator(worker, inputs, code, totals);
    return generator.run();
}

std::optional<CompiledCode> generateEntryWrittenByHand(const CodeGenerationWorker& worker, const CodeGenerationInputs& inputs, const Code& code, NOESCAPE const ScopedLambda<TypeID(unsigned)>& typeAskedOfEntryParameter, CodeGenerationTotals& totals, bool nobodyEnters)
{
    std::optional<Vector<QuestionOfEntry, 4>> questions = nobodyEnters ? std::optional(Vector<QuestionOfEntry, 4>()) : findQuestionsOfEntryWrittenByHand(inputs, worker.types, code.unit(), typeAskedOfEntryParameter);
    if (!questions)
        return std::nullopt;
    worker.arena.reset();
    CurrentWorkScope work(inputs.program, code.function(), "emit"_s);
    FunctionCodeGenerator generator(worker, inputs, code, totals);
    return generator.runEntryWrittenByHand(questions->span(), nobodyEnters);
}

bool hasStraightForm(const CodeGenerationInputs& inputs, CodeUnit unit, const BodyIndex& index)
{
    return StraightEmitter::covers(inputs, unit, index);
}

std::optional<CompiledCode> generateStraightCode(const CodeGenerationWorker& worker, const CodeGenerationInputs& inputs, const Code& code, const BodyIndex& index, CodeGenerationTotals& totals)
{
    worker.arena.reset();
    CurrentWorkScope work(inputs.program, code.function(), "emit"_s);
    FunctionCodeGenerator generator(worker, inputs, code, totals);
    return generator.runStraight(index);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
