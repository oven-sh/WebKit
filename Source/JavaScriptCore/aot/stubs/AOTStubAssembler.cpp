/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubAssembler.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTInstance.h"
#include "AOTStubFrontEnds.h"
#include "AOTStubRegisters.h"
#include "AOTStubsExceptions.h"
#include "AOTStubsPolls.h"
#include "AOTVector.h"
#include "AssemblerPolicy.h"
#include "LinkBuffer.h"
#include "StackAlignment.h"
#include "StackManager.h"
#include <algorithm>
#include <utility>
#include <wtf/DataLog.h>

namespace JSC { namespace AOT {

void readTagsFromInstance(CCallHelpers& jit)
{
#if CPU(X86_64)
    jit.readTagsFromMemory(CCallHelpers::Address(instanceGPR, AOT_INSTANCE_OFFSET(numberTag)), CCallHelpers::Address(instanceGPR, AOT_INSTANCE_OFFSET(notCellMask)));
#else
    UNUSED_PARAM(jit);
#endif
}

// The stubs count bits with popcnt. Every x86-64 processor that Bun runs on has it: -march=nehalem is the floor.
static CPUFeatureSet featuresOfStubs(CPUFeatureSet features)
{
#if CPU(X86_64)
    return features.with(CPUFeature::Popcnt);
#else
    return features;
#endif
}

StubAssembler::StubAssembler(const StubOptions& options, const StubCatalog& catalog)
    : m_options(options)
    , m_catalog(catalog)
    , m_relocations(*this, AssemblerPolicy::forImage(featuresOfStubs(options.targetFeatures)))
{
    readTagsFromInstance(*this);
}

StubRecording StubAssembler::generate(StubKey body)
{
    return generateBody(body, [](StubAssembler& jit) {
        if (isOperationCallHead(jit.key()))
            jit.emitOperationCallHead();
        else
            generatorOf(jit.key().family()).generate(jit);
    });
}

StubRecording StubAssembler::generateBody(StubKey body, const ScopedLambda<void(StubAssembler&)>& generator)
{
    RELEASE_ASSERT(exists(body) && bodyOf(body) == body);
    RELEASE_ASSERT(!m_key.hasFamily());

    m_key = body;
    m_signature = signatureOf(body);
    m_recording = { };
    m_recording.body = body;
    m_allocated = RegisterSet();
    m_clobberedOperands = RegisterSet();
    m_borrowedFPRs = RegisterSet();
    m_savedRegisterStack.clear();
    m_outOfLineParts.clear();

    if (!isOperationCallHead(body))
        align();
    Label start = label();
    generator(*this);
    for (size_t index = 0; index < m_outOfLineParts.size(); ++index)
        std::exchange(m_outOfLineParts[index], nullptr)(*this);
    m_outOfLineParts.clear();
    if (!m_recording.entries.contains(body))
        bindEntryAt(body, start);

    if (!m_savedRegisterStack.isEmpty())
        crashWhileGenerating("a region that saves registers is still open at the end of the body"_s);
    checkHasNoBorrowedFPRs();
    size_t numberOfLegalEntries = 0;
    forEachEntryOf(body, [&](StubKey entry) {
        if (!m_recording.entries.contains(entry))
            crashWhileGenerating("an entry of the body was never bound"_s);
        recordSignatureRegisters(entry);
        ++numberOfLegalEntries;
    });
    RELEASE_ASSERT(numberOfLegalEntries == m_recording.entries.size());

    m_key = StubKey();
    return std::exchange(m_recording, { });
}

void StubAssembler::emitOperationCallHead()
{
    RELEASE_ASSERT(m_signature.immediate);
    uint32_t offset = *key().immediate();
    Entry entry = entryAtOffset(offset);
    for (const OperationPrelude& prelude : operationPreludes()) {
        if (prelude.operation == entry && prelude.emit(*this) == PreludeExit::Returns)
            return;
    }
    jumpToOperationCallOf(entry);
}

void StubAssembler::jumpToOperationCallOf(Entry entry)
{
    recordCall(entry);
    move(TrustedImm32(offsetInRuntimeTable(entry)), *m_signature.immediate);
    jumpToStub(key().withoutImmediate());
}

void StubAssembler::recordSignatureRegisters(StubKey entry)
{
    StubSignature signature = signatureOf(entry);
    if (signature.firstResult)
        recordClobber(signature.firstResult);
    if (signature.secondResult)
        recordClobber(signature.secondResult);
    if (signature.immediate)
        recordClobber(*signature.immediate);
}

void StubAssembler::crashWhileGenerating(ASCIILiteral reason) const
{
    PrintStream& out = WTF::dataFile();
    out.print("AOT: cannot generate the stub "_s);
    AOT::dump(out, m_key);
    out.println(": "_s, reason);
    CRASH();
}

void StubAssembler::checkHasNoBorrowedFPRs() const
{
    if (!m_borrowedFPRs.isEmpty())
        crashWhileGenerating("it leaves the stub with a borrowed floating-point register"_s);
}

bool StubAssembler::isUnclobberedOperand(GPRReg gpr) const
{
    Reg reg(gpr);
    if (m_clobberedOperands.contains(reg, IgnoreVectors))
        return false;
    if (reg == m_signature.firstResult || reg == m_signature.secondResult || m_signature.immediate == gpr)
        return false;
    auto operands = std::span(m_signature.operands).first(m_signature.numberOfOperands);
    return std::ranges::find(operands, reg) != operands.end();
}

bool StubAssembler::isPreservedForCaller(GPRReg reg) const
{
    return isARM64() && m_key.callerFrame() == CallerFrame::Frameless && reg == faultCallerLinkGPR;
}

bool StubAssembler::canAllocate(StubRegister stubRegister) const
{
    GPRReg reg = gprOf(stubRegister);
    return reg != InvalidGPRReg && !m_allocated.contains(Reg(reg), IgnoreVectors) && canWrite(reg);
}

GPRReg StubAssembler::allocateScratch(StubRegister stubRegister)
{
    if (!canAllocate(stubRegister))
        crashWhileGenerating("a scratch register is taken, holds an operand, belongs to the caller, or does not exist on this CPU"_s);
    return reuseScratch(stubRegister);
}

GPRReg StubAssembler::reuseScratch(StubRegister stubRegister)
{
    GPRReg reg = gprOf(stubRegister);
    if (reg == InvalidGPRReg || !canWrite(reg))
        crashWhileGenerating("a reused register holds an operand, belongs to the caller, or does not exist on this CPU"_s);
    m_allocated.add(reg);
    recordClobber(reg);
    return reg;
}

FPRReg StubAssembler::allocateScratch(FPRReg reg)
{
    if (reg == InvalidFPRReg || m_allocated.contains(Reg(reg), IgnoreVectors) || m_borrowedFPRs.contains(Reg(reg), IgnoreVectors))
        crashWhileGenerating("a floating-point scratch register is taken"_s);
    m_allocated.add(reg, IgnoreVectors);
    recordClobber(reg);
    return reg;
}

void StubAssembler::saveFPR(FPRReg fpr, GPRReg gpr)
{
    if (m_borrowedFPRs.contains(Reg(fpr), IgnoreVectors))
        crashWhileGenerating("a borrowed floating-point register is saved twice"_s);
    if (!canWrite(gpr))
        crashWhileGenerating("the register that saves a floating-point register holds an operand or belongs to the caller"_s);
    m_borrowedFPRs.add(fpr, IgnoreVectors);
    recordClobber(gpr);
    moveDoubleTo64(fpr, gpr);
}

void StubAssembler::restoreFPR(FPRReg fpr, GPRReg gpr)
{
    move64ToDouble(gpr, fpr);
    m_borrowedFPRs.remove(fpr);
}

GPRReg StubAssembler::clobberOperand(unsigned index)
{
    GPRReg reg = operand(index);
    if (!isSaved(reg) && isPreservedForCaller(reg))
        crashWhileGenerating("a clobbered operand belongs to the caller"_s);
    m_clobberedOperands.add(reg);
    m_allocated.add(reg);
    recordClobber(reg);
    return reg;
}

GPRReg StubAssembler::clobberRegister(GPRReg reg)
{
    if (reg == InvalidGPRReg)
        crashWhileGenerating("a clobbered register does not exist on this CPU"_s);
    for (unsigned index = 0; index < m_signature.numberOfOperands; ++index) {
        if (m_signature.operands[index] == Reg(reg))
            return clobberOperand(index);
    }
    if (!canWrite(reg))
        crashWhileGenerating("a clobbered register belongs to the caller"_s);
    m_allocated.add(reg);
    recordClobber(reg);
    return reg;
}

void StubAssembler::recordClobber(Reg reg)
{
    if (!isSaved(reg))
        m_recording.clobberedInline.add(reg, IgnoreVectors);
}

void StubAssembler::recordClobbers(RegisterSet registers)
{
    if (!m_savedRegisterStack.isEmpty())
        registers.exclude(m_savedRegisterStack.last());
    m_recording.clobberedInline.merge(registers);
}

void StubAssembler::storePreservedRegisters(std::span<const GPRReg> saved)
{
    subPtr(TrustedImm32(alignedSizeOfRegisters(saved.size())), stackPointerRegister);
    for (size_t i = 0; i + 1 < saved.size(); i += 2)
        storePair64(saved[i], saved[i + 1], Address(stackPointerRegister, i * sizeof(CPURegister)));
    if (saved.size() % 2)
        store64(saved.back(), Address(stackPointerRegister, (saved.size() - 1) * sizeof(CPURegister)));
}

void StubAssembler::loadPreservedRegisters(std::span<const GPRReg> saved)
{
    for (size_t i = 0; i + 1 < saved.size(); i += 2)
        loadPair64(Address(stackPointerRegister, i * sizeof(CPURegister)), saved[i], saved[i + 1]);
    if (saved.size() % 2)
        load64(Address(stackPointerRegister, (saved.size() - 1) * sizeof(CPURegister)), saved.back());
    addPtr(TrustedImm32(alignedSizeOfRegisters(saved.size())), stackPointerRegister);
}

StubAssembler::SavedRegisterArea StubAssembler::storeRegistersClobberedBy(StubCallee callee, bool hasResult, const RegisterSet& lostAnyway)
{
    Vector<Reg, 48> registers;
    registersClobberedBy(callee).forEach([&](Reg reg) {
        if (lostAnyway.contains(reg, IgnoreVectors))
            return;
        if (!hasResult || reg != Reg(GPRInfo::returnValueGPR))
            registers.append(reg);
    });
#if CPU(X86_64)
    unsigned bytes = alignedSizeOfRegisters(registers.size() + 2) - sizeof(CPURegister);
    subPtr(TrustedImm32(bytes), stackPointerRegister);
#else
    unsigned bytes = alignedSizeOfRegisters(registers.size() + 1);
    subPtr(TrustedImm32(bytes), stackPointerRegister);
    storePtr(linkRegister, Address(stackPointerRegister));
#endif
    for (unsigned i = 0; i < registers.size(); ++i) {
        Address address(stackPointerRegister, (i + 1) * sizeof(CPURegister));
#if CPU(ARM64)
        if (i + 1 < registers.size() && registers[i].isGPR() == registers[i + 1].isGPR()) {
            if (registers[i].isGPR())
                storePair64(registers[i].gpr(), registers[i + 1].gpr(), address);
            else
                storePairDouble(registers[i].fpr(), registers[i + 1].fpr(), address);
            ++i;
            continue;
        }
#endif
        if (registers[i].isGPR())
            storePtr(registers[i].gpr(), address);
        else
            storeDouble(registers[i].fpr(), address);
    }
    return { WTF::move(registers), bytes };
}

void StubAssembler::loadSavedRegisters(const SavedRegisterArea& area)
{
    auto& registers = area.registers;
    for (unsigned i = 0; i < registers.size(); ++i) {
        Address address(stackPointerRegister, (i + 1) * sizeof(CPURegister));
#if CPU(ARM64)
        if (i + 1 < registers.size() && registers[i].isGPR() == registers[i + 1].isGPR()) {
            if (registers[i].isGPR())
                loadPair64(address, registers[i].gpr(), registers[i + 1].gpr());
            else
                loadPairDouble(address, registers[i].fpr(), registers[i + 1].fpr());
            ++i;
            continue;
        }
#endif
        if (registers[i].isGPR())
            loadPtr(address, registers[i].gpr());
        else
            loadDouble(address, registers[i].fpr());
    }
#if !CPU(X86_64)
    loadPtr(Address(stackPointerRegister), linkRegister);
#endif
    addPtr(TrustedImm32(area.bytes), stackPointerRegister);
}

void StubAssembler::recordCall(Entry entry)
{
    recordUse(entry);
    m_recording.operationsCalled.set(static_cast<size_t>(entry));
}

void StubAssembler::recordCallee(StubCallee callee)
{
    RegisterSet clobbered = registersClobberedBy(callee);
    if (!m_savedRegisterStack.isEmpty())
        clobbered.exclude(m_savedRegisterStack.last());

    switch (callee) {
    case StubCallee::PreserveMostLeaf:
        m_recording.clobberedInline.merge(clobbered);
        return;
    case StubCallee::HostFunction:
    case StubCallee::CompiledCode:
        m_recording.hasUnknownEffects = true;
        [[fallthrough]];
    case StubCallee::Operation:
        m_recording.clobberedByCallees.merge(clobbered);
        m_recording.leavesStubs = true;
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void StubAssembler::recordCallee(PtrTag tag)
{
    switch (tag) {
    case OperationPtrTag:
        recordCallee(StubCallee::Operation);
        return;
    case HostFunctionPtrTag:
        recordCallee(StubCallee::HostFunction);
        return;
    case JITThunkPtrTag:
        RELEASE_ASSERT_NOT_REACHED();
    case JSEntryPtrTag:
    default:
        recordCallee(StubCallee::CompiledCode);
        return;
    }
}

void StubAssembler::recordFarJump(PtrTag tag)
{
    checkHasNoBorrowedFPRs();
    if (tag == NoPtrTag || tag == ExceptionHandlerPtrTag)
        return;
    RELEASE_ASSERT(m_savedRegisterStack.isEmpty());
    recordCallee(tag);
}

void StubAssembler::emitStackAlignmentCheck()
{
#if CPU(X86_64)
    if (!m_options.validates)
        return;
    Jump isAligned = branchTestPtr(Zero, stackPointerRegister, TrustedImm32(stackAlignmentBytes() - 1));
    breakpoint();
    isAligned.link(this);
#endif
}

CCallHelpers::Call StubAssembler::call(GPRReg target, PtrTag tag)
{
    emitStackAlignmentCheck();
    recordCallee(tag);
    return CCallHelpers::call(target, tag);
}

CCallHelpers::Call StubAssembler::call(Address target, PtrTag tag)
{
    emitStackAlignmentCheck();
    recordCallee(tag);
#if CPU(X86_64)
    CCallHelpers::call(target, tag);
    return Call(m_assembler.label(), Call::None);
#else
    return CCallHelpers::call(target, tag);
#endif
}

void StubAssembler::farJump(GPRReg target, PtrTag tag)
{
    recordFarJump(tag);
    CCallHelpers::farJump(target, tag);
}

void StubAssembler::farJump(Address target, PtrTag tag)
{
    recordFarJump(tag);
    CCallHelpers::farJump(target, tag);
}

void StubAssembler::farJump(BaseIndex target, PtrTag tag)
{
    recordFarJump(tag);
    CCallHelpers::farJump(target, tag);
}

void StubAssembler::addReference(StubKey target, StubReference::Kind kind)
{
    if (!exists(target))
        crashWhileGenerating("it refers to a stub that does not exist"_s);
    RegisterSet savedAround;
    switch (kind) {
    case StubReference::Kind::Jump:
        if (!m_savedRegisterStack.isEmpty() && !neverReturns(target))
            crashWhileGenerating("it jumps to another stub out of a region that saves registers"_s);
        checkHasNoBorrowedFPRs();
        break;
    case StubReference::Kind::Call:
    case StubReference::Kind::Address:
        if (!m_savedRegisterStack.isEmpty())
            savedAround = m_savedRegisterStack.last();
        break;
    case StubReference::Kind::ReturnedAddress:
        break;
    }
    m_recording.references.appendIfNotContains(StubReference { target, kind, savedAround });
}

void StubAssembler::emitStubCall(StubKey target)
{
    addReference(target, StubReference::Kind::Call);
    m_pendingCalls.append({ CCallHelpers::nearCall(), target });
}

void StubAssembler::callStub(StubKey target)
{
    emitStackAlignmentCheck();
    emitStubCall(target);
}

void StubAssembler::linkToStub(Jump jump, StubKey target)
{
    addReference(target, StubReference::Kind::Jump);
    m_pendingJumps.append({ jump, target });
}

void StubAssembler::linkToStub(JumpList& jumps, StubKey target)
{
    for (Jump jump : jumps.jumps())
        linkToStub(jump, target);
    jumps.clear();
}

void StubAssembler::loadStubAddress(StubKey target, GPRReg result)
{
    addReference(target, StubReference::Kind::Address);
    m_pendingStubAddresses.append({ movePCRelativeAddress(result, PCRelativeReach::Near), target });
}

void StubAssembler::returnStubAddress(StubKey target)
{
    addReference(target, StubReference::Kind::ReturnedAddress);
    m_pendingStubAddresses.append({ movePCRelativeAddress(result(), PCRelativeReach::Near), target });
    returnToCaller();
}

void StubAssembler::loadLabelAddress(Label target, GPRReg result)
{
    m_pendingLabelAddresses.append({ movePCRelativeAddress(result, PCRelativeReach::Near), target });
}

void StubAssembler::jumpThroughHandlerTable(BaseIndex tableEntry, std::span<const StubKey> handlers)
{
    for (StubKey handler : handlers)
        addReference(handler, StubReference::Kind::Jump);
    CCallHelpers::farJump(tableEntry, JITThunkPtrTag);
}

void StubAssembler::callThroughHandlerTable(BaseIndex tableEntry, std::span<const StubKey> handlers)
{
    if (!canWrite(tableEntry.index))
        crashWhileGenerating("the index register of a called handler table holds an operand or belongs to the caller"_s);
    recordClobber(tableEntry.index);
    for (StubKey handler : handlers)
        addReference(handler, StubReference::Kind::Call);
    loadPtr(tableEntry, tableEntry.index);
    CCallHelpers::call(tableEntry.index, JITThunkPtrTag);
}

void StubAssembler::bindEntryAt(StubKey entry, Label location)
{
    if (!exists(entry) || bodyOf(entry) != key())
        crashWhileGenerating("it binds an entry of another body"_s);
    if (!m_entryLabels.add(entry.bits(), location).isNewEntry)
        crashWhileGenerating("an entry is bound twice"_s);
    m_boundEntries.append({ entry, location });
    m_recording.entries.append(entry);
}

CCallHelpers::Label StubAssembler::bindReturnLabel(StubFrameKind kind)
{
    RELEASE_ASSERT(kind != StubFrameKind::Plain);
    Label location = label();
    m_boundReturnLabels.append({ location, kind });
    m_recording.returnLabels.append(kind);
    return location;
}

void StubAssembler::loadCallerReturnAddress(unsigned bytesPushed, GPRReg result)
{
#if CPU(X86_64)
    loadPtr(Address(stackPointerRegister, bytesPushed), result);
#else
    UNUSED_PARAM(bytesPushed);
    move(linkRegister, result);
#endif
}

void StubAssembler::returnToCaller()
{
    RELEASE_ASSERT(m_savedRegisterStack.isEmpty());
    checkHasNoBorrowedFPRs();
    RELEASE_ASSERT(!isARM64() || key().callerFrame() != CallerFrame::Frameless);
    ret();
}

void StubAssembler::checkReturnsFlags() const
{
    RELEASE_ASSERT(m_signature.result == StubResult::Flags);
    RELEASE_ASSERT(m_signature.trueCondition == Zero || m_signature.trueCondition == NonZero);
}

void StubAssembler::checkReturnsFlags(RelationalCondition condition) const
{
    checkReturnsFlags();
    RELEASE_ASSERT(condition == Equal || condition == NotEqual);
    RELEASE_ASSERT((condition == Equal) == (m_signature.trueCondition == Zero));
}

void StubAssembler::returnFlags32(RelationalCondition condition, GPRReg left, GPRReg right)
{
    checkReturnsFlags(condition);
    compareOnFlags32(left, right);
    returnToCaller();
}

void StubAssembler::returnFlags32(RelationalCondition condition, GPRReg left, TrustedImm32 right)
{
    checkReturnsFlags(condition);
    compareOnFlags32(left, right);
    returnToCaller();
}

void StubAssembler::returnFlags64(RelationalCondition condition, GPRReg left, GPRReg right)
{
    checkReturnsFlags(condition);
    compareOnFlags64(left, right);
    returnToCaller();
}

void StubAssembler::returnFlagsTest32(GPRReg reg, TrustedImm32 mask)
{
    checkReturnsFlags();
    test32(reg, mask);
    returnToCaller();
}

void StubAssembler::returnFlagsConstant(bool value)
{
    checkReturnsFlags();
    if (value == (m_signature.trueCondition == Zero))
        compareOnFlags64(instanceGPR, instanceGPR);
    else {
#if CPU(X86_64)
        m_assembler.testq_rr(instanceGPR, instanceGPR);
#else
        m_assembler.tst<64>(instanceGPR, instanceGPR);
#endif
    }
    returnToCaller();
}

void StubAssembler::jumpToExceptionHandler()
{
    if (key().callerFrame() == CallerFrame::StubFrame)
        emitFunctionEpilogue();
    jumpToStub(handleException(false));
}

CCallHelpers::Address StubAssembler::limitOfCallsByValue()
{
    return Address(instanceGPR, AOT_INSTANCE_OFFSET(callsByValue.limit) + StackManager::Mirror::offsetOfTrapAwareSoftStackLimit());
}

CCallHelpers::Address StubAssembler::softStackLimitWord()
{
    return Address(instanceGPR, AOT_INSTANCE_OFFSET(stackMirror) + StackManager::Mirror::offsetOfSoftStackLimit());
}

CCallHelpers::Address StubAssembler::trapAwareSoftStackLimitWord()
{
    return Address(instanceGPR, AOT_INSTANCE_OFFSET(stackMirror) + StackManager::Mirror::offsetOfTrapAwareSoftStackLimit());
}

void StubAssembler::checkStackBeforeEntering()
{
    pollIfBelow(trapAwareSoftStackLimitWord(), pollBeforeEntering(PollBeforeEnteringForm::Plain));
}

// Native code that finds no room for what it allocates goes on with what is kept aside. sp is as at the entry of this stub, so what the poll throws is thrown at the site of the call.
// Nothing is entered here, so a stack that is merely used up is no reason to throw.
void StubAssembler::pollBeforeReturningFromNativeCode()
{
    if (key().callerFrame() == CallerFrame::StubFrame)
        return;
    Jump mayOwe = branchPtr(Below, stackPointerRegister, trapAwareSoftStackLimitWord());
    Label afterPoll = label();
    addOutOfLinePart([mayOwe, afterPoll](StubAssembler& jit) {
        mayOwe.link(&jit);
        jit.branchPtr(Below, stackPointerRegister, softStackLimitWord()).linkTo(afterPoll, &jit);
        jit.pollAtStubEntry(pollBeforeEntering(PollBeforeEnteringForm::Plain));
        jit.jump().linkTo(afterPoll, &jit);
    });
}

void StubAssembler::checkStackBeforeEntering(GPRReg endOfFrame)
{
    Jump mayNotFit = branchPtr(Below, endOfFrame, trapAwareSoftStackLimitWord());
    Label afterCheck = label();
    addOutOfLinePart([mayNotFit, afterCheck, endOfFrame](StubAssembler& jit) {
        mayNotFit.link(&jit);
        jit.linkToStub(jit.branchPtr(Below, endOfFrame, softStackLimitWord()), throwStackOverflow());
        jit.pollAtStubEntry(pollBeforeEntering(PollBeforeEnteringForm::Plain));
        jit.jump().linkTo(afterCheck, &jit);
    });
}

void StubAssembler::checkStackBeforeEnteringCallee()
{
    pollIfBelow(limitOfCallsByValue(), pollBeforeEntering(PollBeforeEnteringForm::FunctionObject));
}

void StubAssembler::pollIfBelow(Address limit, StubKey poll)
{
    Jump doesNotFit = branchPtr(Below, stackPointerRegister, limit);
    Label afterCheck = label();
    addOutOfLinePart([doesNotFit, afterCheck, poll](StubAssembler& jit) {
        doesNotFit.link(&jit);
        jit.pollAtStubEntry(poll);
        jit.jump().linkTo(afterCheck, &jit);
    });
}

// sp is as at the entry of this stub: on x86-64 not aligned for a call, which PollBeforeEntering expects.
void StubAssembler::pollAtStubEntry(StubKey poll)
{
#if CPU(ARM64)
    move(linkRegister, ARM64Registers::x16);
#endif
    emitStubCall(poll);
#if CPU(X86_64)
    bindReturnLabel(StubFrameKind::AtStubEntry);
#endif
}

void StubAssembler::pollBeforeCall(Label returnSite)
{
#if CPU(ARM64)
    loadLabelAddress(returnSite, ARM64Registers::x16);
    emitStubCall(pollBeforeEntering(PollBeforeEnteringForm::Plain));
#else
    loadLabelAddress(returnSite, s_scratchRegister);
    push(s_scratchRegister);
    pollAtStubEntry(pollBeforeEntering(PollBeforeEnteringForm::Plain));
    addPtr(TrustedImm32(sizeof(void*)), stackPointerRegister);
#endif
}

CCallHelpers::Label StubAssembler::labelOf(StubKey entry) const
{
    auto iterator = m_entryLabels.find(entry.bits());
    RELEASE_ASSERT(iterator != m_entryLabels.end());
    return iterator->value;
}

void StubAssembler::linkJumpsBetweenStubs()
{
    for (const PendingJump& pending : m_pendingJumps)
        pending.jump.linkTo(labelOf(pending.target), this);
    m_pendingJumps.clear();
}

bool StubAssembler::linkCallsAndAddresses(LinkBuffer& linkBuffer)
{
    for (const PendingCall& pending : m_pendingCalls)
        linkBuffer.link<JITThunkPtrTag>(pending.call, linkBuffer.locationOf<JITThunkPtrTag>(labelOf(pending.target)));
    bool isInReach = true;
    for (const PendingStubAddress& pending : m_pendingStubAddresses)
        isInReach &= linkBuffer.link(pending.site, RelocationKind::AddressNear, labelOf(pending.target));
    for (const PendingLabelAddress& pending : m_pendingLabelAddresses)
        isInReach &= linkBuffer.link(pending.site, RelocationKind::AddressNear, pending.target);
    return isInReach;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
