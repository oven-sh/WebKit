/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTASANShadowChecks.h"
#include "AOTRelocation.h"
#include "AOTRuntimeTable.h"
#include "AOTStubBlob.h"
#include "AOTStubCatalog.h"
#include "AOTStubDescriptor.h"
#include "AOTStubFrames.h"
#include "AOTStubRecording.h"
#include "AOTVector.h"
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
    void callCode(CodeSymbol);

    void callStub(StubKey);
    void jumpToStub(StubKey target) { linkToStub(jump(), target); }
    void linkToStub(Jump, StubKey);
    void linkToStub(JumpList&, StubKey);
    void loadStubAddress(StubKey, GPRReg);
    void returnStubAddress(StubKey);
    void loadLabelAddress(Label, GPRReg);
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

#if AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE
#define AOT_FOR_EACH_STUB_MEMORY_ACCESS(v) \
    v(load8, 1, Read) v(load8SignedExtendTo32, 1, Read) v(load16, 2, Read) v(load16Unaligned, 2, Read) v(load16SignedExtendTo32, 2, Read) \
    v(load32, 4, Read) v(load32SignedExtendTo64, 4, Read) v(load64, 8, Read) v(loadPtr, 8, Read) v(loadFloat, 4, Read) v(loadDouble, 8, Read) \
    v(loadPair64, 16, Read) v(loadVector, 16, Read) \
    v(branch8, 1, Read) v(branch16, 2, Read) v(branch32, 4, Read) v(branch64, 8, Read) v(branchPtr, 8, Read) \
    v(branchTest8, 1, Read) v(branchTest16, 2, Read) v(branchTest32, 4, Read) v(branchTest64, 8, Read) v(branchTestPtr, 8, Read) \
    v(branchTestBit32, 4, Read) v(branchTestBit64, 8, Read) v(compare32, 4, Read) v(test32, 4, Read) \
    v(compareOnFlags32, 4, Read) v(compareOnFlags64, 8, Read) v(testOnFlags64, 8, Read) \
    v(convertInt32ToDouble, 4, Read) v(convertInt64ToDouble, 8, Read) v(convertFloatToDouble, 4, Read) \
    v(addDouble, 8, Read) v(subDouble, 8, Read) v(mulDouble, 8, Read) v(divDouble, 8, Read) v(mul32, 4, Read) \
    v(store8, 1, Write) v(store16, 2, Write) v(store32, 4, Write) v(store64, 8, Write) v(storePtr, 8, Write) v(storeFloat, 4, Write) v(storeDouble, 8, Write) \
    v(storePair64, 16, Write) v(storeValue, 8, Write) \
    v(add8, 1, Update) v(add32, 4, Update) v(add64, 8, Update) v(addPtr, 8, Update) v(sub32, 4, Update) v(sub64, 8, Update) \
    v(and32, 4, Update) v(and64, 8, Update) v(or32, 4, Update) v(or64, 8, Update) v(xor32, 4, Update) v(xor64, 8, Update) \
    v(neg32, 4, Update) v(neg64, 8, Update) v(not32, 4, Update) v(not64, 8, Update) \
    v(branchAdd32, 4, Update) v(branchAdd64, 8, Update) v(branchSub32, 4, Update) v(branchMul32, 4, Update) v(atomicXchg32, 4, Update)

#define AOT_DEFINE_CHECKED_STUB_MEMORY_ACCESS(name, size, kind) \
    template<typename... Arguments> decltype(auto) name(Arguments... arguments) \
    { \
        checkASANShadow<size, MemoryAccess::kind>(arguments...); \
        return CCallHelpers::name(arguments...); \
    }
    AOT_FOR_EACH_STUB_MEMORY_ACCESS(AOT_DEFINE_CHECKED_STUB_MEMORY_ACCESS)
#undef AOT_DEFINE_CHECKED_STUB_MEMORY_ACCESS
#undef AOT_FOR_EACH_STUB_MEMORY_ACCESS

#define AOT_FOR_EACH_STUB_CELL_BYTE_TEST(v) \
    v(branchIfType, typeInfoTypeOffset) v(branchIfNotType, typeInfoTypeOffset) v(branchIfObject, typeInfoTypeOffset) v(branchIfNotObject, typeInfoTypeOffset) \
    v(branchIfString, typeInfoTypeOffset) v(branchIfNotString, typeInfoTypeOffset) v(branchIfSymbol, typeInfoTypeOffset) v(branchIfNotSymbol, typeInfoTypeOffset) \
    v(barrierBranchWithoutFence, cellStateOffset)

#define AOT_DEFINE_CHECKED_STUB_CELL_BYTE_TEST(name, offset) \
    template<typename... Arguments> Jump name(GPRReg cell, Arguments... arguments) \
    { \
        checkASANShadow<1, MemoryAccess::Read>(Address(cell, JSCell::offset())); \
        return CCallHelpers::name(cell, arguments...); \
    }
    AOT_FOR_EACH_STUB_CELL_BYTE_TEST(AOT_DEFINE_CHECKED_STUB_CELL_BYTE_TEST)
#undef AOT_DEFINE_CHECKED_STUB_CELL_BYTE_TEST
#undef AOT_FOR_EACH_STUB_CELL_BYTE_TEST
#endif

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
#if AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE
    enum class MemoryAccess : uint8_t { Read, Write, Update };

    template<unsigned size, MemoryAccess access, typename... Arguments>
    void checkASANShadow(const Arguments&... arguments)
    {
        size_t remaining = sizeof...(Arguments);
        auto check = [&]<typename Argument>(const Argument& argument) {
            --remaining;
            if constexpr (std::is_same_v<Argument, Address> || std::is_same_v<Argument, BaseIndex>)
                emitASANShadowCheck(*this, argument, size, access == MemoryAccess::Write || (access == MemoryAccess::Update && !remaining));
        };
        (check(arguments), ...);
    }
#endif

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

inline constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
inline constexpr GPRReg fp = GPRInfo::callFrameRegister;
inline constexpr GPRReg A0 = gprOf(StubRegister::A0);
inline constexpr GPRReg A1 = gprOf(StubRegister::A1);
inline constexpr GPRReg T11 = gprOf(StubRegister::T11);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
