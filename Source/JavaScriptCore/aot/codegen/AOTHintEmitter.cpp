/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTHintEmitter.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTClosure.h"
#include "AOTCodeRecords.h"
#include "AOTCodeSites.h"
#include "AOTConvention.h"
#include "AOTExceptionEmitter.h"
#include "AOTFunctionKinds.h"
#include "AOTInstance.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTSlimClosure.h"
#include "AOTStubCatalog.h"
#include "AOTStubRegisters.h"
#include "AOTStubsOperations.h"
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
    // Nobody wants the word. The stub that the handler of the fault enters comes back through a scratch register.
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
    // On ARM64 a comparison with zero is a test of the sign bit, which sets no flags first.
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

// Its place among the parameters of entry 0, which is its place in CallingConventions::locationsOf() of the unit.
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

// `x = null` stores to no place that the collector reads: the object stays in whatever register or stack slot held it until something else is put there.
void HintEmitter::generateReleaseHint(const ValueContext& value)
{
    // The sweep counts no hint as a read. Without the mark the operand may have no code, or is read again, or would cross a suspension for the hint alone: nothing here may name it.
    if (m_code.marksOf(value.id()).contains(SweepMark::ReleasesOperand))
        release(value->valueOperand());
}

void HintEmitter::release(ValueID operand)
{
    // No cell is made for such a closure.
    if (isDeferredClosure(operand))
        return;
    // A number, a boolean or a narrow reference is nothing that the collector takes for a pointer.
    Operand released = get(operand);
    if (!released.value || released.value->type() != Machine::Int64 || released.value->hasInt64())
        return;
    // Nor is what lies in the frame a cell that the collector could free.
    ValueID definition = definitionOf(operand);
    const Operation& named = operationAt(definition);
    if (Operation::opcodesWithStorage.contains(named.opcode) && named.storage() != AllocationStorage::Heap)
        return;
    // A parameter that came on the stack is still in the word that the caller stored it to. Nothing reads that word again: this is the store that the program wrote.
    // A function that was suspended runs on above another frame: the word is then somebody else's.
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

// The header names no cell, nor do the order word and the shape word of an object. The stores go by the heaps of the fields, so they stay behind whatever reads a field back.
void HintEmitter::generateClearFrameCell(const ValueContext& value)
{
    const Operation& made = operationAt(definitionOf(value->cellOperand()));
    RELEASE_ASSERT((made.opcode == Opcode::NewObject || made.opcode == Opcode::NewClosure || made.opcode == Opcode::NewArray) && made.storage() == AllocationStorage::Frame);

    // In a realm without a header for such a cell the array lies on the heap, so its elements are found through it. Nobody has the array any more, wherever it lies.
    if (made.opcode == Opcode::NewArray) {
        LValue butterfly = m_out.loadPtr(in<CellPointer>(value->cellOperand()).value(), m_heaps.JSArray_butterfly);
        for (unsigned index = 0; index < made.elementsOperands().size(); ++index)
            m_out.store64(m_out.constInt64(0), butterfly, m_heaps.indexedContiguousProperties[index]);
        return;
    }

    size_t begin = sizeof(JSCell);
    size_t end = 0;
    AbstractHeap heap = AbstractHeap::EnvironmentFields;
    if (made.opcode == Opcode::NewClosure) {
        // The operation lists a capture that is removed, and the cell has no word for it.
        unsigned numberOfWords = closureCaptureCountsOf(m_program, conventions(), made.function()).captures;
        end = made.closureForm() == ClosureForm::Slim ? SlimClosure::allocationSize(numberOfWords) : Closure::allocationSize(numberOfWords);
    } else {
        end = m_plan.layout(made.layout()).size;
        if (isObjectLayout(m_program.at(made.layout()).kind)) {
            begin = offsetOfFirstField;
            heap = AbstractHeap::ObjectFields;
        }
    }

    CellPointer cell = in<CellPointer>(value->cellOperand());
    // Behind the cell lies whatever else the frame holds.
    RELEASE_ASSERT(cell.value()->opcode() != Machine::SlotBase || end <= cell.value()->slot()->byteSize());
    for (size_t offset = begin; offset < end; offset += sizeof(uint64_t))
        m_out.store64(m_out.constInt64(0), m_out.address(m_heaps.entire(heap), cell.value(), offset));
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
    // Handling a trap stops the thread for the collector or ends it. It looks at no object of the program, runs none of its code and looks no name up:
    // of the state of the run time that generated code reads, a poll that comes back has changed what a collection changes.
    effects.writes.remove(AbstractHeapSet { AbstractHeap::RuntimeState });
    emitter<RuntimeCallEmitter>().applyEffects(patchpoint, effects, ArgumentBufferAccess::None, false, RuntimeCallEmitter::CellConversions::None);
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    uint32_t site = emitter<RuntimeCallEmitter>().addSite(value);

    // The walk learns that the poll of a loop leaves its handler when the loop is closed, behind the record.
    bool leavesOwedError = value->leavesOwedError() || m_code.marksOf(value.id()).contains(SweepMark::LeavesOwedError);
    if (hasFaultingLoopPolls) {
        // Without the poll's need, a frame that a block in the loop needs is set up in that block wherever the way back to the head of the loop has a rare edge,
        // as the way out of an inner loop has: in every turn. On ARM64 the frame stays around the loop.
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
