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
    // The body of an async function in which nothing suspends is an ordinary function: it returns and throws to whoever called it.
    bool isBodyThatMaySuspend = isBodyOfCoroutine(unit.role, conventions.isCoroutine(unit.function)) && !conventions.of(unit.function).coroutineNeverSuspends;
    bool savesFrameOnSuspend = code.hasSuspensions() || isBodyThatMaySuspend;
    // The stubs that suspend a function return in the place of its frame and drop nothing above its return address.
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
        .checksAllocation = runsMachineChecks(build),
        .notesRangesOfOrigins = hasFlag(build.options().reports, ReportSet::Flow) || !build.options().logging.mapFilePath.isNull(),
        .features = build.targetFeatures(),
    };
}

static void enterStepOfCodeGeneration(ASCIILiteral step)
{
    currentWorkOfThread().step.store(step.characters(), std::memory_order_relaxed);
}

namespace {

struct OperationsGivenToEmitters {
    uint64_t count { 0 };
    bool prologueMayCollect { false };
};

} // anonymous namespace

// A unit that polls in full at its prologue may collect there, so every call that can enter it has to say that it may collect: AOTStubsPolls.h.
// A call goes by what Facts says of the function. A unit in which no operation may collect does not collect in its prologue either.
static OperationsGivenToEmitters lookAtOperationsGivenToEmitters(const Facts& facts, const Code& code)
{
    OperationsGivenToEmitters operations;
    FunctionID function = code.function();
    bool looksForCollection = facts.effectsOnReturn(function).mayCollect || facts.effectsOnThrow(function).mayCollect;
    uint32_t numberOfBlocks = code.numberOfBlocks();
    for (uint32_t index = 0; index < numberOfBlocks; ++index) {
        BlockID block(index);
        if (!code.isReached(block))
            continue;
        std::span<const ValueID> values = code.valuesOf(block);
        operations.count += values.size();
        if (!looksForCollection)
            continue;
        for (ValueID value : values) {
            if (!code.at(value).traits().mayCollect())
                continue;
            operations.prologueMayCollect = true;
            looksForCollection = false;
            break;
        }
    }
    return operations;
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
    case Machine::FailureKind::RegisterLacksItsValue:
        return InternalErrorKind::RegisterLacksItsValue;
    case Machine::FailureKind::SpillSlotLacksItsValue:
        return InternalErrorKind::SpillSlotLacksItsValue;
    case Machine::FailureKind::FrameAddressAcrossSuspension:
        return InternalErrorKind::FrameAddressAcrossSuspension;
    case Machine::FailureKind::NarrowReferenceWithoutPointer:
        return InternalErrorKind::NarrowReferenceWithoutPointer;
    case Machine::FailureKind::InnerPointerWithoutOwner:
        return InternalErrorKind::InnerPointerWithoutOwner;
    case Machine::FailureKind::NeedsFrameWhereNoneIsSetUp:
        return InternalErrorKind::FrameNeededWhereNoneIsSetUp;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void reportFailureOfMachineProcedure(BuildContext& build, const Program& program, const Code& code, const Machine::Procedure& procedure)
{
    const Machine::Failure& failure = *procedure.failure();

    StringPrintStream text;
    text.print(failure.kind);
    if (failure.operation < procedure.numberOfOperations()) {
        text.print(": ");
        Machine::printOperation(text, procedure, procedure.operationAt(failure.operation));
    }

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
        // Without a location the numbers of a record are looked up in the body as the front end wrote it, where those of Code mean nothing.
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
    , m_relocations(m_jit, worker.build.assemblerPolicy())
    , m_records(inputs.stubCatalog, m_relocations, totals, &worker.arena.objectKeptAcrossResets<RememberedStubKeys>())
    , m_procedure(worker.arena, inputs.heaps.tree(), machineTraitsOf(worker.build, inputs, code))
{
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
    OperationsGivenToEmitters operations = lookAtOperationsGivenToEmitters(m_inputs.facts, m_code);
    m_stackChecks.functionMayCollect = operations.prologueMayCollect;

    // Whether a unit leaves from argument list frames is settled before its code is generated.
    bool unitReadsCallerFrame = readsCallerFrame(m_inputs, m_code);
    m_records.frame.readsCallerFrame = unitReadsCallerFrame;

    CodeGenerator generator(FunctionInputs { build, m_worker.types, m_inputs, m_code }, m_procedure, m_records);
    if (generator.run() == GenerationResult::Failed)
        return false;
    if (m_procedure.failure()) {
        reportFailureOfMachineProcedure(build, m_inputs.program, m_code, m_procedure);
        return false;
    }
    RELEASE_ASSERT(m_records.frame.savesFrameOnSuspend <= m_procedure.traits().savesFrameOnSuspend);
    RELEASE_ASSERT(m_records.frame.readsCallerFrame == unitReadsCallerFrame);

    build.statistics().count(StatisticID::OperationsEmitted, operations.count);
    return true;
}

// The procedure holds one terminal patch, which takes the arguments where they arrive and writes the whole body. So the prologue, the stack test, the entries, the link and the description are those of every unit.
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
    return code;
}

CompiledCode FunctionCodeGenerator::emitCode()
{
    BuildContext& build = m_worker.build;
    Statistics& statistics = build.statistics();

    enterStepOfCodeGeneration("allocate"_s);
    // As generateStackCheckAtFrameSetup() goes.
    if (m_stackChecks.stackCheck == StackCheck::CoveredByCallers && !m_records.frame.needsStackCheckForTailCalls)
        m_procedure.setLargestFrameSetUpWithoutStackCheck(maxFrameSizeWithoutStackCheck);
    Machine::prepareForGeneration(m_procedure);
    if (m_procedure.failure()) {
        reportFailureOfMachineProcedure(build, m_inputs.program, m_code, m_procedure);
        return { };
    }
    if (!frameFitsImage())
        return { };

    const Machine::Allocation& allocation = m_procedure.allocation();
    // Order appends too: a Jump for each edge that it splits, and the Upsilons of that edge again.
    statistics.count(StatisticID::MachineOperationsAppended, m_procedure.numberOfOperations());
    statistics.count(StatisticID::MachineOperationsScanned, countMachineOperationsInOrder(m_procedure.order()));
    statistics.count(StatisticID::MachineOperationsAllocated, allocation.numberOfOperationsAllocated());
    statistics.count(StatisticID::ValuesSpilled, allocation.spills().size());

    enterStepOfCodeGeneration("encode"_s);
    readTagsFromInstance(m_jit);
    CCallHelpers::Jump fromFusedCallEntry = emitEntryPreambles(m_jit, m_inputs, m_code.unit(), m_records);
    CCallHelpers::Label bodyStart = m_jit.label();
    Machine::generate(m_procedure, m_jit, m_relocations);
    if (m_straightEmitter && m_straightEmitter->hasFailed())
        return { };
    if (m_procedure.failure()) {
        reportFailureOfMachineProcedure(build, m_inputs.program, m_code, m_procedure);
        return { };
    }
    // The preamble that spills the argument registers falls through to the primary entry, and so do the secondary entries and a fused call entry, but that of a guard.
    const CallingConventions& conventions = m_inputs.plan.conventions();
    bool holdsFusedCallEntry = conventions.holdsFusedCallEntry(m_code.unit());
    bool isGuard = m_code.unit().role == CodeRole::ParameterGuard;
    bool preambleFallsThrough = m_records.frame.spillsArgumentRegisters || !conventions.secondaryEntriesOf(m_code.unit()).empty() || (holdsFusedCallEntry && !isGuard);
    RELEASE_ASSERT(!preambleFallsThrough || !CCallHelpers::differenceBetween(bodyStart, m_procedure.entrypointLabel(0)));
    if (fromFusedCallEntry.isSet())
        fromFusedCallEntry.linkTo(m_procedure.entrypointLabel(0), &m_jit);
    else if (holdsFusedCallEntry && isGuard)
        m_records.entries.append(RecordedEntry { fusedCallEntry, m_procedure.entrypointLabel(0) });
    statistics.count(StatisticID::MachineOperationsEncoded, m_procedure.numberOfOperationsEncoded());

    emitStackOverflowPaths(m_jit, m_records, m_stackChecks);
    // The return address of a call at the very end would lie in the code that follows.
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
