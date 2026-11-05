/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTHintEmitter.h"

#if ENABLE(AOT)

#include "AOTAllocationEmitter.h"
#include "AOTCodeRecords.h"
#include "AOTCodeSites.h"
#include "AOTCompileUnit.h"
#include "AOTConvention.h"
#include "AOTExceptionEmitter.h"
#include "AOTInstance.h"
#include "AOTPoisonedCell.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTStubCatalog.h"
#include "AOTStubRegisters.h"
#include "AOTStubsOperations.h"
#include "AOTThrowEmitter.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "LinkBuffer.h"
#include "StackManager.h"
#include <algorithm>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

static constexpr ptrdiff_t offsetOfTrapAwareSoftStackLimit = AOT_INSTANCE_OFFSET(stackMirror) + StackManager::Mirror::offsetOfTrapAwareSoftStackLimit();
static_assert(static_cast<int64_t>(StackManager::StopRequestMarkerValue) < 0);

static constexpr StubKey pollSlowKey = callOperationPreservingRegisters({ .returns = OperationCall::Returns::Void, .saves = OperationCall::Saves::CallerSavedRegisters });

static void emitFaultingLoad(CCallHelpers& jit, const Machine::PatchParameters& params, CodeRecords* records, uint32_t site, bool leavesOwedError)
{
    AllowMacroScratchRegisterUsage allowScratch(jit);
    bool isFrameless = !params.frameIsSetUp();
    CCallHelpers::Label label = jit.labelIgnoringWatchpoints();
    size_t start = jit.m_assembler.codeSize();
    ptrdiff_t offset = InstanceLayout::offsetOfLoopPollPage() + (isFrameless ? offsetOfLeafPollInLoopPollPage : 0) + (leavesOwedError ? offsetOfPollLeavingOwedErrorInLoopPollPage : 0);
#if CPU(ARM64)
    static_assert(sizeOfWordLoadedByLoopPoll == sizeof(uint64_t));
    RELEASE_ASSERT(ARM64Assembler::canEncodePImmOffset<64>(static_cast<int32_t>(offset)));
    jit.m_assembler.ldr<64>(ARM64Registers::zr, instanceGPR, static_cast<unsigned>(offset));
#else
    jit.load32(CCallHelpers::Address(instanceGPR, offset), jit.scratchRegister());
#endif
    RELEASE_ASSERT(jit.m_assembler.codeSize() - start == sizeOfLoopPoll);
    jit.addLinkTask([=](LinkBuffer& linkBuffer) {
        records->traps.pollSites.append(PollSite { linkBuffer.offsetOf(label), isFrameless, OptionalIndex { site } });
    });
}

static void emitCompareAndCall(CCallHelpers& jit, const Machine::PatchParameters& params, CodeRecords* records, uint32_t site, bool leavesOwedError)
{
    AllowMacroScratchRegisterUsage allowScratch(jit);
    RELEASE_ASSERT(params.frameIsSetUp());
    CCallHelpers::Jump isInterrupted = jit.branch64(CCallHelpers::LessThan, CCallHelpers::Address(instanceGPR, offsetOfTrapAwareSoftStackLimit), CCallHelpers::TrustedImm32(0));
    CCallHelpers::Label done = jit.label();
    params.addLatePath([=](CCallHelpers& jit) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        isInterrupted.link(&jit);
        if (leavesOwedError)
            jit.store32(CCallHelpers::TrustedImm32(1), CCallHelpers::Address(instanceGPR, AOT_INSTANCE_OFFSET(runtime.exceptions.pollLeavesOwedError)));
        records->stubCalls.call(jit, pollSlowKey, offsetInRuntimeTable(AOT::Entry::operationAOTPollSlow), OptionalIndex { site });
        jit.jump().linkTo(done, &jit);
    });
}

static std::optional<size_t> findEntryParameterNamedByHint(const Code& code, ValueID definition)
{
    if (code.at(definition).opcode != Opcode::Parameter)
        return std::nullopt;
    std::span<const ValueID> parameters = code.at(code.entries()[primaryEntry].block).parameters();
    auto found = std::ranges::find(parameters, definition);
    if (found == parameters.end())
        return std::nullopt;
    return static_cast<size_t>(found - parameters.begin());
}

void HintEmitter::generateLoopHint(const ValueContext& value)
{
    emitPoll(value);
}

void HintEmitter::generateReleaseHint(const ValueContext& value)
{
    if (m_code.marksOf(value.id()).contains(SweepMark::ReleasesOperand))
        release(value->valueOperand());
}

void HintEmitter::release(ValueID operand)
{
    if (isDeferredClosure(operand))
        return;
    Operand released = get(operand);
    if (!released.value || released.value->type() != Machine::Int64 || released.value->hasInt64())
        return;
    ValueID definition = definitionOf(operand);
    const Operation& named = operationAt(definition);
    if (Operation::opcodesWithStorage.contains(named.opcode) && named.storage() != AllocationStorage::Heap)
        return;
    if (conventions().entryConventionOf(m_unit) == EntryConvention::Direct && !functionSuspends()) {
        if (std::optional<size_t> parameter = findEntryParameterNamedByHint(m_code, definition)) {
            ArgumentLocation location = conventions().locationsOf(m_program, m_unit).parameters[*parameter];
            if (location.kind == ArgumentLocation::Kind::Stack)
                m_out.store64(m_out.constInt64(0), m_out.address(m_heaps.argumentBuffer, m_out.framePointer(), offsetOfStackWordFromFrame(location.stackIndex)));
        }
    }
    Machine::PatchOperation* patchpoint = m_out.patchpoint(Machine::Void);
    patchpoint->append(released.value, Machine::ValueRep::ColdAny);
    patchpoint->effects = Machine::Effects::none();
    patchpoint->effects.writesLocalState = true;
    patchpoint->setReleasesDeadValues(true);
    patchpoint->setGenerator([](CCallHelpers& jit, const Machine::PatchParameters& params) {
        RegisterSet registers = params.value()->registersToRelease();
        registers.exclude(params.usedRegisters());
        registers.forEach([&](Reg reg) {
            jit.move(CCallHelpers::TrustedImm32(0), reg.gpr());
        });
    });
}

void HintEmitter::generateEndLifeOfCell(const ValueContext& value)
{
    endLifeOf(value, value->cellOperand());
}

bool HintEmitter::endsLivesAtRunTime()
{
    return sourcesOfProofsCheckedUnder(build().options().testing) & bitOfSourceOfProofs(ProofSource::Lifetime);
}

void HintEmitter::endLifeOf(const OperationContext& context, ValueID cell)
{
    const Operation& made = operationAt(definitionOf(cell));
    if (made.opcode == Opcode::NewObject && made.storage() == AllocationStorage::Frame)
        return emitter<AllocationEmitter>().clearCellsThatObjectLentHolds(definitionOf(cell));
    FunctionID madeIn = generator().code().inlineFrames()[made.inlineFrame].callee;
    m_out.suspendDeclaredAccesses();
    callVoidOperation(context, RuntimeOperation::PoisonCell, { in<CellPointer>(cell), word32(static_cast<int32_t>(madeIn.index())), word32(static_cast<int32_t>(made.position)) });
    m_out.resumeDeclaredAccesses();
}

void HintEmitter::noteThatCellIsLent(const ValueContext& made, CellPointer cell)
{
    FunctionID madeIn = generator().code().inlineFrames()[made->inlineFrame].callee;
    m_out.suspendDeclaredAccesses();
    callVoidOperation(made, RuntimeOperation::PoisonCell, { cell, word32(static_cast<int32_t>(madeIn.index() | PoisonedCell::isLentForCallAhead)), word32(static_cast<int32_t>(made->position)) });
    m_out.resumeDeclaredAccesses();
}

void HintEmitter::stopWhereLifeHasEnded(const OperationContext& context, CellPointer cell)
{
    if (!endsLivesAtRunTime())
        return;
    Word32 type(m_out.load8ZeroExt32(m_out.address(m_heaps.collectorState, cell.value(), JSCell::typeInfoTypeOffset())));
    emitter<ThrowEmitter>().requireOrStopAtFalseProof(context, notEqual(type, word32(ObjectType)), boxCell(cell), ProofSource::Lifetime);
}

void HintEmitter::pollWhereNoFrameIsSetUp(const OperationContext& context)
{
    RELEASE_ASSERT(hasFaultingLoopPolls);
    Machine::PatchOperation* patchpoint = m_out.patchpoint(Machine::Void);
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->setRunsWithoutFrame(true);
    uint32_t site = emitter<RuntimeCallEmitter>().addSite(context);
    patchpoint->setGenerator([records = &m_records, site](CCallHelpers& jit, const Machine::PatchParameters& params) {
        if (!params.frameIsSetUp())
            emitFaultingLoad(jit, params, records, site, false);
    });
}

void HintEmitter::emitPoll(const ValueContext& value)
{
    Machine::PatchOperation* patchpoint = m_out.patchpoint(Machine::Void);
    Effects effects = effectsOf(value);
    RELEASE_ASSERT(effects.mayBeTerminated && effects.mayThrow.isDeclaredOnlyForLackOfMemory());
    effects.writes.remove(AbstractHeapSet { AbstractHeap::RuntimeState });
    emitter<RuntimeCallEmitter>().applyEffects(patchpoint, effects, ArgumentBufferAccess::None, false, RuntimeCallEmitter::CellConversions::None);
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    uint32_t site = emitter<RuntimeCallEmitter>().addSite(value);

    bool leavesOwedError = m_code.marksOf(value.id()).contains(SweepMark::LeavesOwedError);
    if (hasFaultingLoopPolls) {
        patchpoint->setRunsWithoutFrame(!isARM64());
        patchpoint->setGenerator([records = &m_records, site, leavesOwedError](CCallHelpers& jit, const Machine::PatchParameters& params) {
            emitFaultingLoad(jit, params, records, site, leavesOwedError);
        });
        emitter<ExceptionEmitter>().attachLandingPad(value, patchpoint, site, true);
        return;
    }

    patchpoint->clobberLate(inputs().stubCatalog.summaryOf(pollSlowKey).clobbered);
    m_records.totals.entriesUsed.set(static_cast<unsigned>(AOT::Entry::operationAOTPollSlow));
    patchpoint->setGenerator([records = &m_records, site, leavesOwedError](CCallHelpers& jit, const Machine::PatchParameters& params) {
        emitCompareAndCall(jit, params, records, site, leavesOwedError);
    });
    emitter<ExceptionEmitter>().attachLandingPad(value, patchpoint, site, true);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
