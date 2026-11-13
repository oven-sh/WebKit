/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRelocation.h"
#include "AOTRuntimeTable.h"
#include "AOTStubBlob.h"
#include "AOTStubCatalog.h"
#include "AOTStubDescriptor.h"
#include "AOTStubFrames.h"
#include "AOTStubRecording.h"
#include "AOTVector.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "CCallHelpers.h"
#include "DisallowMacroScratchRegisterUsage.h"
#include "StackAlignment.h"
#include <array>
#include <initializer_list>
#include <optional>
#include <span>
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/Function.h>
#include <wtf/HashMap.h>
#include <wtf/MathExtras.h>
#include <wtf/ScopedLambda.h>
#include <wtf/Variant.h>

namespace JSC {

class LinkBuffer;

namespace AOT {

struct OperationCall {
    enum class Returns : uint8_t { Value, Void, Double, Pair };
    enum class FirstArgument : uint8_t { Instance, GlobalObject, VM, None };
    enum class Saves : uint8_t { Nothing, CallerSavedGPRs, CallerSavedGPRsAndFewFPRs, CallerSavedRegisters };

    struct SpilledArguments {
        unsigned count;
        uint32_t countWord;
    };

    struct ConstantArgument {
        GPRReg argument;
        uint32_t value;
    };

    Returns returns { Returns::Value };
    FirstArgument firstArgument { FirstArgument::Instance };
    Saves saves { Saves::Nothing };
    bool checksException { true };
    bool pollsBeforeReturning { false };
    std::optional<GPRReg> secondArgumentIn { };
    std::optional<GPRReg> thirdArgumentIn { };
    std::optional<GPRReg> fourthArgumentIn { };
    std::optional<GPRReg> fifthArgumentIn { };
    std::array<std::optional<ConstantArgument>, 2> constantArguments { };
    std::optional<GPRReg> valueDestination { };
    std::optional<SpilledArguments> spilledArguments { };
};

constexpr bool mayFailToAllocate(Entry entry, OperationCall::FirstArgument firstArgument, const Typed::Effects& effects)
{
    switch (entry) {
    case Entry::operationAOTCatch:
    case Entry::operationAOTCatchException:
    case Entry::operationAOTCatchValueAndException:
        return false;
    default:
        return firstArgument == OperationCall::FirstArgument::Instance && effects.mayCollect;
    }
}

constexpr bool mayFailToAllocate(Entry entry, OperationCall::FirstArgument firstArgument)
{
    return mayFailToAllocate(entry, firstArgument, effectsOf(entry));
}

struct PreservingCall {
    StubCallee callee { StubCallee::CompiledCode };
    bool returnsValue { true };
    bool checksException { true };
    std::array<std::optional<GPRReg>, 3> cellsToKeep { };
    const ScopedLambda<void()>* emitBeforeReturn { nullptr };
    std::span<const GPRReg> registersThatCalleeLoses { };
};

class OwnedScratchRegister {
    WTF_MAKE_NONCOPYABLE(OwnedScratchRegister);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    explicit OwnedScratchRegister(CCallHelpers& jit)
        : m_register(jit.scratchRegister())
    {
#if CPU(X86_64)
        m_forbidsAssembler.emplace(jit);
#endif
    }

    OwnedScratchRegister(CCallHelpers& jit, GPRReg handedOn)
        : m_register(handedOn)
    {
        if (handedOn != registerToHandOn)
            return;
        jit.scratchRegister();
#if CPU(X86_64)
        m_forbidsAssembler.emplace(jit);
#endif
    }

#if CPU(X86_64)
    static constexpr GPRReg registerToHandOn = CCallHelpers::s_scratchRegister;
#else
    static constexpr GPRReg registerToHandOn = CCallHelpers::dataTempRegister;
#endif

    GPRReg gpr() const { return m_register; }
    operator GPRReg() const { return m_register; }

private:
    GPRReg m_register;
#if CPU(X86_64)
    std::optional<DisallowMacroScratchRegisterUsage<CCallHelpers>> m_forbidsAssembler;
#endif
};

void readTagsFromInstance(CCallHelpers&);
RegisterSet floatingPointRegistersLostToAccessors(StubCallee);

enum class SavedRegistersAreRead : bool { ByCallerAlone, BeforeReturnToo };

class StubAssembler final : public CCallHelpers {
    WTF_FORBID_HEAP_ALLOCATION;
public:
    StubAssembler(const StubOptions&, const StubCatalog&, PCRelativeReach addressReach = PCRelativeReach::Near);

    StubRecording generate(StubKey body);
    StubRecording generateForTesting(StubKey body, const ScopedLambda<void(StubAssembler&)>& generator) { return generateBody(body, generator); }

    StubKey key() const;
    bool callerKeepsNoFPRs() const { return m_key.callerFrame() == CallerFrame::KeepsNoFPRs; }
    const StubSignature& signature() const { return m_signature; }
    const StubOptions& options() const { return m_options; }
    const StubPolicy& bindingPolicy() const { return m_catalog.policy(); }
    RelocationRecorder& relocations() { return m_relocations; }

    GPRReg operand(unsigned) const;
    FPRReg operandFPR(unsigned) const;
    GPRReg result() const;
    GPRReg secondResult() const;
    FPRReg resultFPR() const;
    GPRReg immediate() const;

    GPRReg scratchRegister() = delete;
    GPRReg allocateScratch(StubRegister);
    FPRReg allocateScratch(FPRReg);
    void saveFPR(FPRReg, GPRReg);
    void restoreFPR(FPRReg, GPRReg);
    GPRReg clobberOperand(unsigned);
    GPRReg reuseScratch(StubRegister);
    GPRReg savedScratch(StubRegister);
    GPRReg clobberRegister(GPRReg);
    GPRReg clobberRegister(StubRegister stubRegister) { return clobberRegister(gprOf(stubRegister)); }
    bool canAllocate(StubRegister) const;
    void recordClobbers(RegisterSet);

    template<typename Functor> void preserving(std::span<const GPRReg>, const Functor&);
    template<typename Functor> void preserving(std::initializer_list<GPRReg> saved, const Functor& functor) { preserving(std::span<const GPRReg>(saved.begin(), saved.end()), functor); }
    template<typename Functor> void preservingRegistersClobberedBy(StubCallee callee, bool hasResult, const Functor& functor) { preservingRegistersClobberedBy(callee, hasResult, { }, functor); }
    template<typename Functor> void preservingRegistersClobberedBy(StubCallee, bool hasResult, const RegisterSet& lostAnyway, const Functor&);
    template<typename Functor> void preservingCallerSavedRegisters(bool hasResult, const Functor& functor) { preservingCallerSavedRegisters(hasResult, { }, functor); }
    template<typename Functor> void preservingCallerSavedRegisters(bool hasResult, const RegisterSet& lostAnyway, const Functor&);
    template<typename Functor> void withSavedRegisters(const RegisterSet&, const Functor&);
    void callAndReturnPreservingRegisters(const PreservingCall&, const ScopedLambda<void()>& emitCall);

    void loadOperation(Entry, GPRReg);
    void callOperation(Entry, GPRReg scratch);
    void loadRuntimePointer(Entry, GPRReg);
    void callOperationAndReturn(Entry entry, const OperationCall& operationCall) { callOperationAndReturn(entry, operationCall, [] { }, SavedRegistersAreRead::ByCallerAlone); }
    void callOperationAndReturn(Entry, const OperationCall&, const ScopedLambda<void()>& beforeReturn, SavedRegistersAreRead = SavedRegistersAreRead::BeforeReturnToo);
    void callOperationAndReturn(GPRReg offsetInRuntimeTable, const OperationCall& operationCall) { emitOperationCallAndReturn(offsetInRuntimeTable, operationCall, [] { }, SavedRegistersAreRead::ByCallerAlone); }
    void tailCallOperation(Entry entry, OperationCall::FirstArgument firstArgument) { emitOperationTailCall(entry, firstArgument); }
    void tailCallOperation(GPRReg offsetInRuntimeTable, OperationCall::FirstArgument firstArgument) { emitOperationTailCall(offsetInRuntimeTable, firstArgument); }
    void callLeaf(Entry, unsigned bytesPushed = 0);
    void jumpToOperationCallOf(Entry);
    void jumpToCallOfOperation(Entry);
    void jumpToStubTakingImmediate();

    void exitOnMiss(Entry cachingOperation, const OperationCall&);
    void exitOnCannot(Entry finisher, const OperationCall&);


    void callStub(StubKey);
    void jumpToStub(StubKey target) { linkToStub(jump(), target); }
    void linkToStub(Jump, StubKey);
    void linkToStub(JumpList&, StubKey);
    void loadStubAddress(StubKey, GPRReg);
    void returnStubAddress(StubKey);
    void loadLabelAddress(Label, GPRReg);
    void jumpThroughHandlerTable(BaseIndex tableEntry, std::span<const StubKey> handlers);
    void callThroughHandlerTable(BaseIndex tableEntry, std::span<const StubKey> handlers, GPRReg scratch);

    void align();
    void alignWhatStandsBehind(unsigned size);
    void bindEntry(StubKey entry) { bindEntryAt(entry, label()); }
    void bindEntryBehindItsHead(StubKey);
    Label bindReturnLabel(StubFrameKind);

    void returnToCaller();
    void boxBooleanAtEveryReturn() { m_boxesBooleanAtEveryReturn = true; }
    void goOnAtEveryReturnIn(std::optional<StubKey> stub) { m_stubBehindEveryReturn = stub; }
#if CPU(X86_64)
    void emitFramelessCallerEpilogue();
#else
    void emitFramelessCallerEpilogueAndReturn(GPRReg scratch);
#endif
    void loadCallerReturnAddress(unsigned bytesPushed, GPRReg result);
    void jumpToExceptionHandler();
    void checkStackBeforeEntering();
    void pollBeforeReturningFromNativeCode();
    void pollBeforeReturningFromOperation(Label afterPoll);
    Jump branchIfNonZeroOrPollIsDue(GPRReg value, GPRReg scratch);
    void checkStackBeforeEntering(GPRReg endOfFrame);
    void checkStackBeforeEnteringCallee();
    CallPosition callPosition() const { return m_callPosition; }
    void setCallPosition(CallPosition position) { m_callPosition = position; }
    void pollBeforeCall(Label returnSite);
    static Address limitOfTailCallsByValue();
    static Address softStackLimitWord();
    static Address pollIsDueWord();
    static Address trapAwareSoftStackLimitWord();

    void returnFlags32(RelationalCondition, GPRReg, GPRReg);
    void returnFlags32(RelationalCondition, GPRReg, TrustedImm32);
    void returnFlags64(RelationalCondition, GPRReg, GPRReg);
    void returnFlagsTest32(GPRReg reg) { returnFlagsTest32(reg, TrustedImm32(-1)); }
    void returnFlagsTest32(GPRReg, TrustedImm32 mask);
    void returnFlagsConstant(bool);

    void addOutOfLinePart(Function<void(StubAssembler&)>&& part) { m_outOfLineParts.append(WTF::move(part)); }

    Call call(GPRReg, PtrTag);
    Call call(Address, PtrTag);
    void farJump(GPRReg, PtrTag);
    void farJump(Address, PtrTag);
    void farJump(BaseIndex, PtrTag);
    Call nearCall() = delete;
    Call nearTailCall() = delete;
    Call threadSafePatchableNearCall() = delete;

    struct BoundEntry {
        StubKey key;
        Label label;
    };
    struct BoundReturnLabel {
        Label label;
        StubFrameKind kind;
    };
    const Vector<BoundEntry>& boundEntries() const { return m_boundEntries; }
    const Vector<BoundReturnLabel>& boundReturnLabels() const { return m_boundReturnLabels; }
    void linkJumpsBetweenStubs();
    [[nodiscard]] bool linkCallsAndAddresses(LinkBuffer&);

private:
    using CalledOperation = Variant<Entry, GPRReg>;

    struct SavedRegisterArea {
        Vector<Reg, 48> registers;
        unsigned bytes;
    };
    struct PendingJump {
        Jump jump;
        StubKey target;
    };
    struct PendingCall {
        Call call;
        StubKey target;
    };
    struct PendingStubAddress {
        Label site;
        StubKey target;
    };
    struct PendingLabelAddress {
        Label site;
        Label target;
    };

    StubRecording generateBody(StubKey body, const ScopedLambda<void(StubAssembler&)>& generator);
    void emitOperationCallHead();
    void recordSignatureRegisters(StubKey entry);
    NO_RETURN_DUE_TO_CRASH void crashWhileGenerating(ASCIILiteral reason) const;
    void checkHasNoBorrowedFPRs() const;

    static constexpr unsigned alignedSizeOfRegisters(size_t numberOfRegisters) { return WTF::roundUpToMultipleOf<stackAlignmentBytes()>(numberOfRegisters * sizeof(CPURegister)); }

    bool isSaved(Reg reg) const { return !m_savedRegisterStack.isEmpty() && m_savedRegisterStack.last().contains(reg, IgnoreVectors); }
    bool isUnclobberedOperand(GPRReg) const;
    bool isPreservedForCaller(GPRReg) const;
    bool canWrite(GPRReg reg) const { return isSaved(reg) || (!isUnclobberedOperand(reg) && !isPreservedForCaller(reg)); }
    void recordClobber(Reg);
    void recordUse(Entry entry) { m_recording.entriesUsed.set(static_cast<size_t>(entry)); }
    void recordCall(Entry);
    void recordCallee(StubCallee);
    void recordCallee(PtrTag);
    void recordFarJump(PtrTag);
    void addReference(StubKey, StubReference::Kind);

    void storePreservedRegisters(std::span<const GPRReg>);
    void loadPreservedRegisters(std::span<const GPRReg>);
    enum class StackPointerAfterLoads : bool { IsMovedBack, IsLeftToEpilogue };
    SavedRegisterArea storeRegistersClobberedBy(StubCallee, bool hasResult, const RegisterSet& lostAnyway, std::span<const std::optional<GPRReg>> cellsToKeep = { }, unsigned bytesLeftFreeAbove = 0);
    void loadSavedRegisters(const SavedRegisterArea&, StackPointerAfterLoads = StackPointerAfterLoads::IsMovedBack);

    void loadEntry(Entry, GPRReg);
    void loadCalledOperation(const CalledOperation&, GPRReg function);
    void loadFirstArgument(OperationCall::FirstArgument);
#if CPU(X86_64)
    using OperationToCall = Address;
#else
    using OperationToCall = GPRReg;
#endif
    OperationToCall setUpOperationCall(const CalledOperation&, const OperationCall&);
    void prepareCallOperation();
    void emitOperationCallAndReturn(const CalledOperation&, const OperationCall&, const ScopedLambda<void()>& beforeReturn, SavedRegistersAreRead);
    bool resultsInFPRsComeFromOperation(const OperationCall&) const;
    void emitOperationCallSavingNothing(const CalledOperation&, const OperationCall&, const ScopedLambda<void()>& beforeReturn);
    void emitOperationCallSavingRegisters(const CalledOperation&, const OperationCall&, const ScopedLambda<void()>& beforeReturn);
    void emitOperationTailCall(const CalledOperation&, OperationCall::FirstArgument);

    void emitStubCall(StubKey, StubReference::Kind = StubReference::Kind::Call);
    void pollIfBelow(Address limit, StubKey poll);
    void pollAtStubEntry(StubKey poll, StubReference::Kind = StubReference::Kind::Call);
    void bindEntryAt(StubKey, Label);
    Label labelOf(StubKey entry) const;

    const StubOptions m_options;
    const StubCatalog& m_catalog;
    RelocationRecorder m_relocations;

    StubKey m_key;
    CallPosition m_callPosition { CallPosition::Ordinary };
    StubSignature m_signature;
    StubRecording m_recording;
    RegisterSet m_allocated;
    RegisterSet m_clobberedOperands;
    RegisterSet m_borrowedFPRs;
    Vector<RegisterSet, 4> m_savedRegisterStack;
    bool m_boxesBooleanAtEveryReturn { false };
    std::optional<StubKey> m_stubBehindEveryReturn;
    Vector<Function<void(StubAssembler&)>> m_outOfLineParts;

    Vector<BoundEntry> m_boundEntries;
    Vector<BoundReturnLabel> m_boundReturnLabels;
    HashMap<uint64_t, Label> m_entryLabels;
    Label m_headOfBody;
    Vector<StubKey, 2> m_entriesBehindTheirHeads;
    Vector<PendingJump> m_pendingJumps;
    Vector<PendingCall> m_pendingCalls;
    Vector<PendingStubAddress> m_pendingStubAddresses;
    Vector<PendingLabelAddress> m_pendingLabelAddresses;
    PCRelativeReach m_addressReach;
};

inline StubKey StubAssembler::key() const
{
    ASSERT(m_key.hasFamily());
    return callerKeepsNoFPRs() ? m_key.withCallerFrame(CallerFrame::HasFrame) : m_key;
}

inline GPRReg StubAssembler::operand(unsigned index) const
{
    RELEASE_ASSERT(index < m_signature.numberOfOperands && m_signature.operands[index].isGPR());
    return m_signature.operands[index].gpr();
}

inline FPRReg StubAssembler::operandFPR(unsigned index) const
{
    RELEASE_ASSERT(index < m_signature.numberOfOperands && m_signature.operands[index].isFPR());
    return m_signature.operands[index].fpr();
}

inline GPRReg StubAssembler::result() const
{
    RELEASE_ASSERT(m_signature.result == StubResult::GPR || m_signature.result == StubResult::TwoGPRs);
    return m_signature.firstResult.gpr();
}

inline GPRReg StubAssembler::secondResult() const
{
    RELEASE_ASSERT(m_signature.result == StubResult::TwoGPRs);
    return m_signature.secondResult.gpr();
}

inline FPRReg StubAssembler::resultFPR() const
{
    RELEASE_ASSERT(m_signature.result == StubResult::FPR);
    return m_signature.firstResult.fpr();
}

inline GPRReg StubAssembler::immediate() const
{
    RELEASE_ASSERT(m_signature.immediate && !key().immediate());
    return *m_signature.immediate;
}

template<typename Functor>
void StubAssembler::withSavedRegisters(const RegisterSet& saved, const Functor& functor)
{
    RegisterSet allSaved = saved;
    if (!m_savedRegisterStack.isEmpty())
        allSaved.merge(m_savedRegisterStack.last());
    m_savedRegisterStack.append(allSaved);
    functor();
    m_savedRegisterStack.removeLast();
}

template<typename Functor>
void StubAssembler::preserving(std::span<const GPRReg> saved, const Functor& functor)
{
    if (saved.empty()) {
        functor();
        return;
    }
    storePreservedRegisters(saved);
    withSavedRegisters(RegisterSet::fromIterable(saved), functor);
    loadPreservedRegisters(saved);
}

template<typename Functor>
void StubAssembler::preservingCallerSavedRegisters(bool hasResult, const RegisterSet& lostAnyway, const Functor& functor)
{
#if CPU(X86_64)
    emitFunctionPrologue();
    subPtr(TrustedImm32(sizeof(CPURegister)), stackPointerRegister);
    preservingRegistersClobberedBy(StubCallee::Operation, hasResult, lostAnyway, [&](unsigned bytes) {
        functor(bytes + 2 * static_cast<unsigned>(sizeof(CPURegister)));
    });
    emitFunctionEpilogue();
#else
    preservingRegistersClobberedBy(StubCallee::Operation, hasResult, lostAnyway, functor);
#endif
}

template<typename Functor>
void StubAssembler::preservingRegistersClobberedBy(StubCallee callee, bool hasResult, const RegisterSet& lostAnyway, const Functor& functor)
{
    SavedRegisterArea area = storeRegistersClobberedBy(callee, hasResult, lostAnyway);
    withSavedRegisters(RegisterSet::fromIterable(area.registers), [&] {
        functor(area.bytes);
    });
    loadSavedRegisters(area);
}

inline constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
inline constexpr GPRReg fp = GPRInfo::callFrameRegister;
inline constexpr GPRReg A0 = gprOf(StubRegister::A0);
inline constexpr GPRReg A1 = gprOf(StubRegister::A1);
inline constexpr GPRReg T11 = gprOf(StubRegister::T11);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
