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
#include "CCallHelpers.h"
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
#include <wtf/Vector.h>

namespace JSC {

class LinkBuffer;

namespace AOT {

struct OperationCall {
    enum class Returns : uint8_t { Value, Void, Double, Pair };
    enum class FirstArgument : uint8_t { Instance, GlobalObject, VM, None };
    enum class Saves : uint8_t { Nothing, CallerSavedGPRs, CallerSavedRegisters };

    // operationArgumentGPR(1 .. count) are passed in memory: the second argument is countWord, the third their address.
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
    // Set after the registers are saved, so a call that saves them leaves the argument registers as they were.
    std::optional<GPRReg> secondArgumentIn { };
    std::optional<GPRReg> thirdArgumentIn { };
    std::optional<GPRReg> fourthArgumentIn { };
    std::optional<GPRReg> fifthArgumentIn { };
    std::array<std::optional<ConstantArgument>, 2> constantArguments { };
    std::optional<GPRReg> valueDestination { };
    std::optional<SpilledArguments> spilledArguments { };
};

struct PreservingCall {
    StubCallee callee { StubCallee::CompiledCode };
    bool returnsValue { true };
    bool checksException { true };
};

void readTagsFromInstance(CCallHelpers&);

class StubAssembler final : public CCallHelpers {
    WTF_FORBID_HEAP_ALLOCATION;
public:
    StubAssembler(const StubOptions&, const StubCatalog&);

    StubRecording generate(StubKey body);
    StubRecording generateForTesting(StubKey body, const ScopedLambda<void(StubAssembler&)>& generator) { return generateBody(body, generator); }

    StubKey key() const;
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

    GPRReg allocateScratch(StubRegister);
    FPRReg allocateScratch(FPRReg);
    // The register is not clobbered: every path restores it before an exit. The check follows emission order, not control flow.
    void saveFPR(FPRReg, GPRReg);
    void restoreFPR(FPRReg, GPRReg);
    GPRReg clobberOperand(unsigned);
    GPRReg reuseScratch(StubRegister);
    // clobberOperand for an operand of the signature, reuseScratch for any other register.
    GPRReg clobberRegister(GPRReg);
    GPRReg clobberRegister(StubRegister stubRegister) { return clobberRegister(gprOf(stubRegister)); }
    bool canAllocate(StubRegister) const;
    // For code that another generator emits through the base class.
    void recordClobbers(RegisterSet);

    template<typename Functor> void preserving(std::span<const GPRReg>, const Functor&);
    template<typename Functor> void preserving(std::initializer_list<GPRReg> saved, const Functor& functor) { preserving(std::span<const GPRReg>(saved.begin(), saved.end()), functor); }
    template<typename Functor> void preservingRegistersClobberedBy(StubCallee callee, bool hasResult, const Functor& functor) { preservingRegistersClobberedBy(callee, hasResult, { }, functor); }
    // What is in lostAnyway is neither stored nor loaded.
    template<typename Functor> void preservingRegistersClobberedBy(StubCallee, bool hasResult, const RegisterSet& lostAnyway, const Functor&);
    // For a stub that has no frame record, with the stack as at its entry.
    template<typename Functor> void preservingCallerSavedRegisters(bool hasResult, const Functor&);
    template<typename Functor> void withSavedRegisters(const RegisterSet&, const Functor&);
    void callAndReturnPreservingRegisters(const PreservingCall&, const ScopedLambda<void()>& emitCall);

    void loadOperation(Entry, GPRReg);
    void callOperation(Entry, GPRReg scratch);
    void loadRuntimePointer(Entry, GPRReg);
    void callOperationAndReturn(Entry entry, const OperationCall& operationCall) { callOperationAndReturn(entry, operationCall, [] { }); }
    void callOperationAndReturn(Entry, const OperationCall&, const ScopedLambda<void()>& beforeReturn);
    void callOperationAndReturn(GPRReg offsetInRuntimeTable, const OperationCall& operationCall) { emitOperationCallAndReturn(offsetInRuntimeTable, operationCall, [] { }); }
    void tailCallOperation(Entry entry, OperationCall::FirstArgument firstArgument) { emitOperationTailCall(entry, firstArgument); }
    void tailCallOperation(GPRReg offsetInRuntimeTable, OperationCall::FirstArgument firstArgument) { emitOperationTailCall(offsetInRuntimeTable, firstArgument); }
    void callLeaf(Entry, unsigned bytesPushed = 0);

    void exitOnMiss(Entry cachingOperation, const OperationCall&);
    void exitOnCannot(Entry finisher, RuntimeCounterID reason, GPRReg reasonArgument, const OperationCall&);

    void callEngineThunk(Entry pointer, GPRReg scratch);
    void jumpToEngineThunk(Entry pointer, GPRReg scratch);
    void callCode(CodeSymbol);

    void callStub(StubKey);
    void jumpToStub(StubKey target) { linkToStub(jump(), target); }
    void linkToStub(Jump, StubKey);
    void linkToStub(JumpList&, StubKey);
    void loadStubAddress(StubKey, GPRReg);
    void returnStubAddress(StubKey);
    void loadLabelAddress(Label, GPRReg);
    void callStubAndReturn(StubKey, const ScopedLambda<void()>& setUpOperands);
    void jumpThroughHandlerTable(BaseIndex tableEntry, std::span<const StubKey> handlers);
    void callThroughHandlerTable(BaseIndex tableEntry, std::span<const StubKey> handlers);

    void bindEntry(StubKey entry) { bindEntryAt(entry, label()); }
    Label bindReturnLabel(StubFrameKind);

    void returnToCaller();
#if CPU(X86_64)
    void emitFramelessCallerEpilogue();
#endif
    void loadCallerReturnAddress(unsigned bytesPushed, GPRReg result);
    void jumpToExceptionHandler();
    void checkStackBeforeEntering();

    void returnFlags32(RelationalCondition, GPRReg, GPRReg);
    void returnFlags32(RelationalCondition, GPRReg, TrustedImm32);
    void returnFlags64(RelationalCondition, GPRReg, GPRReg);
    void returnFlagsTest32(GPRReg reg) { returnFlagsTest32(reg, TrustedImm32(-1)); }
    void returnFlagsTest32(GPRReg, TrustedImm32 mask);
    void returnFlagsConstant(bool);

    void addOutOfLinePart(Function<void(StubAssembler&)>&& part) { m_outOfLineParts.append(WTF::move(part)); }

    // These hide the base's, so that no transfer of control goes unrecorded.
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
    // An entry, or the register that has the offset of an entry in the run-time table.
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
    SavedRegisterArea storeRegistersClobberedBy(StubCallee, bool hasResult, const RegisterSet& lostAnyway);
    void loadSavedRegisters(const SavedRegisterArea&);

    void loadEntry(Entry, GPRReg);
    void loadCalledOperation(const CalledOperation&, GPRReg function);
    void loadFirstArgument(OperationCall::FirstArgument);
    GPRReg setUpOperationCall(const CalledOperation&, const OperationCall&);
    void prepareCallOperation();
    void emitOperationCallAndReturn(const CalledOperation&, const OperationCall&, const ScopedLambda<void()>& beforeReturn);
    void emitOperationCallSavingNothing(const CalledOperation&, const OperationCall&, const ScopedLambda<void()>& beforeReturn);
    void emitOperationCallSavingRegisters(const CalledOperation&, const OperationCall&, const ScopedLambda<void()>& beforeReturn);
    void emitOperationTailCall(const CalledOperation&, OperationCall::FirstArgument);

    void emitStackAlignmentCheck();
    void emitStubCall(StubKey);
    void bindEntryAt(StubKey, Label);
    Label labelOf(StubKey entry) const;
    void checkReturnsFlags(RelationalCondition) const;
    void checkReturnsFlags() const;

    const StubOptions m_options;
    const StubCatalog& m_catalog;
    RelocationRecorder m_relocations;

    StubKey m_key;
    StubSignature m_signature;
    StubRecording m_recording;
    RegisterSet m_allocated;
    RegisterSet m_clobberedOperands;
    RegisterSet m_borrowedFPRs;
    Vector<RegisterSet, 4> m_savedRegisterStack;
    Vector<Function<void(StubAssembler&)>> m_outOfLineParts;

    Vector<BoundEntry> m_boundEntries;
    Vector<BoundReturnLabel> m_boundReturnLabels;
    HashMap<uint64_t, Label> m_entryLabels;
    Vector<PendingJump> m_pendingJumps;
    Vector<PendingCall> m_pendingCalls;
    Vector<PendingStubAddress> m_pendingStubAddresses;
    Vector<PendingLabelAddress> m_pendingLabelAddresses;
};

inline StubKey StubAssembler::key() const
{
    ASSERT(m_key.hasFamily());
    return m_key;
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
void StubAssembler::preservingCallerSavedRegisters(bool hasResult, const Functor& functor)
{
#if CPU(X86_64)
    emitFunctionPrologue();
    // The size of the save area aligns the stack of a stub that has pushed nothing, and the prologue pushed one register.
    subPtr(TrustedImm32(sizeof(CPURegister)), stackPointerRegister);
    preservingRegistersClobberedBy(StubCallee::Operation, hasResult, [&](unsigned bytes) {
        functor(bytes + 2 * static_cast<unsigned>(sizeof(CPURegister)));
    });
    emitFunctionEpilogue();
#else
    preservingRegistersClobberedBy(StubCallee::Operation, hasResult, functor);
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

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
