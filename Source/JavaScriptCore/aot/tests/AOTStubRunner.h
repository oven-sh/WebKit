/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTClosureWord.h"
#include "AOTCompileOptionWords.h"
#include "AOTConvention.h"
#include "AOTImageTestSupport.h"
#include "AOTInstance.h"
#include "AOTInstanceLayout.h"
#include "AOTNativeTest.h"
#include "AOTPropertyCachesStringKeys.h"
#include "AOTStubBlob.h"
#include "AOTStubCatalog.h"
#include "AOTStubTestSupport.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTVector.h"
#include "BaselineJITRegisters.h"
#include "CCallHelpers.h"
#include "CallFrame.h"
#include "JITOperationList.h"
#include "JITOperationValidation.h"
#include "JSCJSValueInlines.h"
#include "LinkBuffer.h"
#include "LocalAllocator.h"
#include "StackAlignment.h"
#include "UGPRPair.h"
#include "VM.h"
#include <algorithm>
#include <array>
#include <bit>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <wtf/HashFunctions.h>

#if CPU(X86_64) && OS(LINUX)
#include <signal.h>
#include <ucontext.h>
#define AOT_STUB_RUNNER_COUNTS_INSTRUCTIONS 1
#else
#define AOT_STUB_RUNNER_COUNTS_INSTRUCTIONS 0
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

inline constexpr auto markedFPRs = [] {
    std::array<FPRReg, static_cast<size_t>(MacroAssembler::fpTempRegister)> registers { };
    for (size_t i = 0; i < registers.size(); ++i)
        registers[i] = static_cast<FPRReg>(i);
    return registers;
}();

template<size_t size>
struct alignas(16) FakeObject {
    template<typename T> T& at(ptrdiff_t offset) { return *reinterpret_cast<T*>(bytes.data() + offset); }
    uint64_t address() const { return std::bit_cast<uintptr_t>(bytes.data()); }

    void setHeader(uint32_t structureID, JSType type)
    {
        at<uint32_t>(JSCell::structureIDOffset()) = structureID;
        at<uint8_t>(JSCell::typeInfoTypeOffset()) = type;
    }

    std::array<uint8_t, size> bytes { };
};

struct FakeAllocator : FakeObject<sizeof(LocalAllocator)> {
    static constexpr uintptr_t noNextInterval = 1;

    explicit FakeAllocator(uint32_t cellSize)
    {
        nextInterval() = noNextInterval;
        at<uint32_t>(LocalAllocator::offsetOfCellSize()) = cellSize;
    }

    void makeRoomAt(uint64_t start, size_t size)
    {
        intervalStart() = start;
        intervalEnd() = start + size;
    }

    bool hasRoom() { return intervalStart() < intervalEnd(); }

    uintptr_t& intervalStart() { return at<uintptr_t>(LocalAllocator::offsetOfFreeList() + FreeList::offsetOfIntervalStart()); }
    uintptr_t& intervalEnd() { return at<uintptr_t>(LocalAllocator::offsetOfFreeList() + FreeList::offsetOfIntervalEnd()); }
    uintptr_t& nextInterval() { return at<uintptr_t>(LocalAllocator::offsetOfFreeList() + FreeList::offsetOfNextInterval()); }
};

using RecordedArguments = std::array<uint64_t, 5>;
using ArgumentRegisters = std::array<uint64_t, numberOfArgumentGPRs>;

struct CallRegisterValues {
    uint64_t callee;
    uint64_t thisValue;
    ArgumentRegisters arguments;
};

struct LandedCall {
    enum class Place : uint64_t { Nowhere, CompiledFunction, EngineCallee };

    Place place;
    uint64_t count;
    uint64_t calleeInRegister;
    CallRegisterValues values;
};

struct StubEnvironment {
    static StubEnvironment& of(Instance* instance) { return *reinterpret_cast<StubEnvironment*>(reinterpret_cast<uint8_t*>(instance) - OBJECT_OFFSETOF(StubEnvironment, instance)); }

    static constexpr size_t bytesBelowInstance = WTF::roundUpToMultipleOf<InstanceLayout::granuleBelowInstance>(static_cast<size_t>(std::max({ static_cast<ptrdiff_t>(sizeof(InstancePrefix)), -InstanceLayout::offsetOfKeyedValuePlaces, -InstanceLayout::offsetOfMissesOfPlaces, -Instance::offsetOfBoxedCalls() })));

    std::array<uint8_t, bytesBelowInstance - sizeof(InstancePrefix)> tablesAndUnusedBelowPrefix;
    InstancePrefix prefix;
    Instance instance;
    std::array<uint8_t, InstanceLayout::offsetOfStringCells - sizeof(Instance)> unusedBelowStringCells;
    std::array<uint32_t, 16> stringCells;
    ZeroedStorage<VM> vm;
    std::array<void*, numberOfEntries> runtimeTable;
    void* stub;
    std::array<uint64_t, 6> operands;
    uint64_t immediate;
    std::array<uint64_t, markedFPRs.size()> numbers;
    uint64_t result;
    std::array<uint64_t, markedFPRs.size()> numbersAfterCall;
    std::array<uint64_t, allocatableGPRs.size()> registersAfterCall;
    unsigned numberOfOperationCalls;
    unsigned numberOfRecordedCalls;
    RecordedArguments recordedArguments;
    uint64_t resultOfRecordedOperation;
    CallRegisterValues call;
    LandedCall landedCall;
    uint64_t resultOfNextLanding;
    uint64_t calleeOfEarlierLanding;
    uint64_t thisValueOfEarlierLanding;
    uint64_t wordsNobodyWrote;
    std::array<AbsenceFilter, AbsenceFilter::numberOfFilters> absenceFilters;
    std::array<uint8_t, MissesOfPairs::numberOfCounters> missesOfPairs;
    std::array<uint8_t, 0x2000> emptyStructures;
    std::array<uint8_t, AccessTable::alignment + sizeof(AccessTable)> roomForAccessTable;
};
static_assert(OBJECT_OFFSETOF(StubEnvironment, instance) == OBJECT_OFFSETOF(StubEnvironment, prefix) + sizeof(InstancePrefix) && OBJECT_OFFSETOF(StubEnvironment, instance) == StubEnvironment::bytesBelowInstance);
static_assert(OBJECT_OFFSETOF(StubEnvironment, stringCells) == OBJECT_OFFSETOF(StubEnvironment, instance) + InstanceLayout::offsetOfStringCells && sizeof(uint32_t) == InstanceLayout::sizeOfStringCell);

struct StubRun {
    uint64_t result;
    bool calledOperation;
    unsigned numberOfRecordedCalls;
    RecordedArguments recordedArguments;
    LandedCall landedCall;
    std::optional<uint32_t> instructionsInStubs { };
    RegisterSet changedRegisters { };
    std::optional<uint32_t> leapsInStubs { };
};

class StubRunner {
public:
    static constexpr uint64_t unsetArgument = 0xbadbadbadbadULL;
    static constexpr uint64_t resultOfCompiledFunction = 0x1111;
    static constexpr uint64_t resultOfEngineCallee = 0x2222;

    StubRunner();
    explicit StubRunner(const StubOptions&);
    explicit StubRunner(std::span<const StubKey> keysWithBoundImmediates);
    StubRunner(StubPolicy&&, std::span<const StubKey> keysWithBoundImmediates);

    StubRun run(StubKey, std::initializer_list<uint64_t> operands, std::optional<uint32_t> immediate = std::nullopt);
    StubRun runOperationCall(StubKey, std::initializer_list<uint64_t> arguments);
    StubRun runCall(StubKey, uint64_t callee, uint64_t thisValue, std::initializer_list<uint64_t> arguments, std::optional<uint32_t> immediate = std::nullopt);
    uint64_t closureWord(unsigned numberOfParameters, ClosureWord::Flags flags = { }) const { return ClosureWord::encode({ distanceOfCompiledFunction, numberOfParameters }, 1, flags); }
    Instance& instance() { return m_environment->instance; }
    void forgetAccessTable() { instance().propertyCaches.accessTable.forgetEverything(); }
    uint32_t& stringCell(uint32_t number) { return m_environment->stringCells[number]; }
    void* entryOf(StubKey key) const { return addressOf(key); }

    template<typename Function>
    void install(Entry entry, Function* function) { m_environment->runtimeTable[indexOf(entry)] = operationPointer(function); }
    void countCallsOf(Entry entry) { install(entry, countOperationCall); }
    void recordCallsOf(Entry entry) { install(entry, recordOperationCall); }
    void setResultOfRecordedOperation(uint64_t result) { m_environment->resultOfRecordedOperation = result; }
    void setResultOfNextLanding(uint64_t result) { m_environment->resultOfNextLanding = result; }
    uint64_t calleeOfEarlierLanding() const { return m_environment->calleeOfEarlierLanding; }
    uint64_t thisValueOfEarlierLanding() const { return m_environment->thisValueOfEarlierLanding; }
    void countWordsNobodyWroteAtCallsOf(Entry entry) { install(entry, std::bit_cast<UGPRPair (SYSV_ABI *)(Instance*)>(m_landingThatCountsWordsNobodyWrote)); }
    void countWordsNobodyWroteAndLeaveStubAtCallsOf(Entry entry)
    {
        install(entry, std::bit_cast<UGPRPair (SYSV_ABI *)(Instance*)>(m_landingThatCountsWordsNobodyWroteAndLeavesStub));
        m_landingMayLeaveStub = true;
    }
    uint64_t wordsNobodyWrote() const { return m_environment->wordsNobodyWrote; }
    void setCountsInstructions(bool counts) { m_countsInstructions = counts; }

private:
    StubRunner(const StubOptions&, std::span<const StubKey> keysWithBoundImmediates, StubPolicy&& = { });

    static constexpr uint64_t distanceOfCompiledFunction = 64;
    static constexpr uint64_t wordThatNobodyWrote = 0x5ca1ab1e5ca1ab1eULL;
    static constexpr unsigned numberOfWordsMarkedBelowCaller = 96;

    template<typename Function>
    static void* operationPointer(Function* function)
    {
#if ENABLE(JIT_OPERATION_VALIDATION)
        JITOperationAnnotation annotation { std::bit_cast<void*>(function), std::bit_cast<void*>(function) };
        JITOperationList::populatePointersInEmbedder(&annotation, &annotation + 1);
#endif
        return tagCFunctionPtr<void*, OperationPtrTag>(function);
    }

    static UGPRPair JIT_OPERATION_ATTRIBUTES countOperationCall(Instance*);
    static UGPRPair JIT_OPERATION_ATTRIBUTES recordOperationCall(Instance*, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    static StubBlob buildBlob(const StubCatalog&, std::span<const StubKey> keysWithBoundImmediates);
    static CCallHelpers::Address environmentSlot(size_t offsetInEnvironment, size_t index = 0);
    static constexpr uint64_t markOfRegister(size_t indexInAllocatableGPRs) { return 0x6d61726b00000000ULL + indexInAllocatableGPRs; }
    static std::optional<GPRReg> registerThatHoldsAddressOfStub(const StubSignature&, bool passesImmediate);
    static void emitStubCall(CCallHelpers&, const StubSignature&, bool passesImmediate);
    static MacroAssemblerCodeRef<JSEntryPtrTag> compileStubCaller(const StubSignature&, bool passesImmediate);
    static void emitLandingInCompiledFunction(CCallHelpers&);
    static void emitLandingInEngineCallee(CCallHelpers&);
    static void emitLandingThatCountsWordsNobodyWrote(CCallHelpers&, bool leavesStub);
    void compileLandings();
    void* addressOf(StubKey) const;
    StubRun run(StubKey, const StubSignature&, std::initializer_list<uint64_t> operands, std::optional<uint32_t> immediate);

    StubCatalog m_catalog;
    StubBlob m_blob;
    ImageTests::ExecutableCopy m_code;
    MacroAssemblerCodeRef<JSEntryPtrTag> m_landings;
    void* m_landingThatCountsWordsNobodyWrote { nullptr };
    void* m_landingThatCountsWordsNobodyWroteAndLeavesStub { nullptr };
    bool m_landingMayLeaveStub { false };
    std::unique_ptr<StubEnvironment> m_environment;
    bool m_countsInstructions { false };
};

#if AOT_STUB_RUNNER_COUNTS_INSTRUCTIONS
class InstructionCounter {
    WTF_MAKE_NONCOPYABLE(InstructionCounter);
public:
    InstructionCounter(const uint8_t* start, size_t size)
    {
        s_start = std::bit_cast<uintptr_t>(start);
        s_size = size;
        s_count = 0;
        s_leaps = 0;
        s_previous = 0;
        struct sigaction action { };
        action.sa_sigaction = didStep;
        action.sa_flags = SA_SIGINFO;
        sigemptyset(&action.sa_mask);
        AOT_TEST_CHECK(!sigaction(SIGTRAP, &action, &m_previousAction));
    }

    ~InstructionCounter() { AOT_TEST_CHECK(!sigaction(SIGTRAP, &m_previousAction, nullptr)); }

    static ALWAYS_INLINE void start() { asm volatile("subq $128, %%rsp\n\tpushfq\n\torq $0x100, (%%rsp)\n\tpopfq\n\taddq $128, %%rsp" ::: "memory", "cc"); }
    static ALWAYS_INLINE void stop() { asm volatile("subq $128, %%rsp\n\tpushfq\n\tandq $~0x100, (%%rsp)\n\tpopfq\n\taddq $128, %%rsp" ::: "memory", "cc"); }
    static uint32_t count() { return s_count; }
    static uint32_t leaps() { return s_leaps; }

private:
    static void didStep(int, siginfo_t*, void* context)
    {
        uintptr_t next = static_cast<uintptr_t>(static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP]);
        constexpr uintptr_t longestInstruction = 15;
        if (next - s_start < s_size) {
            ++s_count;
            if (s_previous - s_start < s_size && next - s_previous > longestInstruction)
                ++s_leaps;
        }
        s_previous = next;
    }

    static inline uintptr_t s_start;
    static inline size_t s_size;
    static inline uint32_t s_count;
    static inline uint32_t s_leaps;
    static inline uintptr_t s_previous;
    struct sigaction m_previousAction { };
};
#endif

inline UGPRPair JIT_OPERATION_ATTRIBUTES StubRunner::countOperationCall(Instance* instance)
{
    ++StubEnvironment::of(instance).numberOfOperationCalls;
    return makeUGPRPair(0, 0);
}

inline UGPRPair JIT_OPERATION_ATTRIBUTES StubRunner::recordOperationCall(Instance* instance, uint64_t second, uint64_t third, uint64_t fourth, uint64_t fifth, uint64_t sixth)
{
    StubEnvironment& environment = StubEnvironment::of(instance);
    ++environment.numberOfOperationCalls;
    ++environment.numberOfRecordedCalls;
    environment.recordedArguments = { second, third, fourth, fifth, sixth };
    return makeUGPRPair(environment.resultOfRecordedOperation, 0);
}

inline StubBlob StubRunner::buildBlob(const StubCatalog& catalog, std::span<const StubKey> keysWithBoundImmediates)
{
    WTF::BitSet<numberOfEntries> everyEntry;
    everyEntry.setAll();
    Vector<StubKey> calledKeys = stubKeysWithLinkage(StubLinkage::CalledByCode);
    calledKeys.append(keysWithBoundImmediates);
    BuildContext context(makeDefaultCompileOptions());
    auto blob = buildStubBlob(context, catalog, calledKeys.span(), everyEntry);
    AOT_TEST_CHECK(blob.has_value());
    return WTF::move(*blob);
}

inline CCallHelpers::Address StubRunner::environmentSlot(size_t offsetInEnvironment, size_t index)
{
    size_t offset = offsetInEnvironment + index * sizeof(uint64_t) - OBJECT_OFFSETOF(StubEnvironment, instance);
    return CCallHelpers::Address(instanceGPR, static_cast<int32_t>(offset));
}

inline std::optional<GPRReg> StubRunner::registerThatHoldsAddressOfStub(const StubSignature& signature, bool passesImmediate)
{
    bool takesScratchRegister = passesImmediate && containsRegister(RegisterSet::macroClobberedGPRs(), *signature.immediate);
    for (unsigned i = 0; i < signature.numberOfOperands; ++i)
        takesScratchRegister |= containsRegister(RegisterSet::macroClobberedGPRs(), signature.operands[i]);
    if (!isARM64() || !takesScratchRegister)
        return std::nullopt;
    GPRReg reg = namesRegister(signature, gprOf(StubRegister::T15)) ? gprOf(StubRegister::T14) : gprOf(StubRegister::T15);
    AOT_TEST_CHECK(!namesRegister(signature, reg) && !signature.usesCallConvention);
    return reg;
}

inline void StubRunner::emitStubCall(CCallHelpers& jit, const StubSignature& signature, bool passesImmediate)
{
    std::optional<GPRReg> addressOfStub = registerThatHoldsAddressOfStub(signature, passesImmediate);
    jit.move(CCallHelpers::TrustedImm64(wordThatNobodyWrote), GPRInfo::regT0);
    for (unsigned i = 1; i <= numberOfWordsMarkedBelowCaller; ++i)
        jit.store64(GPRInfo::regT0, CCallHelpers::Address(CCallHelpers::stackPointerRegister, -static_cast<int32_t>(i * sizeof(CPURegister))));
    jit.move(CCallHelpers::TrustedImm32(0), GPRInfo::regT0);
#if CPU(ARM64)
    jit.emitMaterializeTagCheckRegisters();
#endif
    jit.loadPtr(CCallHelpers::Address(instanceGPR, AOT_INSTANCE_OFFSET(cellCageBase)), cellCageBaseGPR);
    for (size_t i = 0; i < allocatableGPRs.size(); ++i)
        jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(markOfRegister(i))), allocatableGPRs[i]);
    if (signature.usesCallConvention) {
        jit.load64(environmentSlot(OBJECT_OFFSETOF(StubEnvironment, call.callee)), calleeGPR);
        jit.load64(environmentSlot(OBJECT_OFFSETOF(StubEnvironment, call.thisValue)), thisGPR);
        for (unsigned i = 0; i < numberOfArgumentGPRs; ++i)
            jit.load64(environmentSlot(OBJECT_OFFSETOF(StubEnvironment, call.arguments), i), argumentGPR(i));
    }
    for (unsigned i = 0; i < signature.numberOfOperands; ++i) {
        if (signature.operands[i].isGPR())
            jit.load64(environmentSlot(OBJECT_OFFSETOF(StubEnvironment, operands), i), signature.operands[i].gpr());
    }
    if (passesImmediate && !addressOfStub)
        jit.load64(environmentSlot(OBJECT_OFFSETOF(StubEnvironment, immediate)), *signature.immediate);
    for (size_t i = 0; i < markedFPRs.size(); ++i)
        jit.loadDouble(environmentSlot(OBJECT_OFFSETOF(StubEnvironment, numbers), i), markedFPRs[i]);
    for (unsigned i = 0; i < signature.numberOfOperands; ++i) {
        if (signature.operands[i].isFPR())
            jit.loadDouble(environmentSlot(OBJECT_OFFSETOF(StubEnvironment, operands), i), signature.operands[i].fpr());
    }

    if (addressOfStub) {
        jit.loadPtr(environmentSlot(OBJECT_OFFSETOF(StubEnvironment, stub)), *addressOfStub);
        if (passesImmediate)
            jit.load64(environmentSlot(OBJECT_OFFSETOF(StubEnvironment, immediate)), *signature.immediate);
        for (unsigned i = 0; i < signature.numberOfOperands; ++i) {
            if (containsRegister(RegisterSet::macroClobberedGPRs(), signature.operands[i]))
                jit.load64(environmentSlot(OBJECT_OFFSETOF(StubEnvironment, operands), i), signature.operands[i].gpr());
        }
        jit.call(*addressOfStub, JITThunkPtrTag);
    } else
        jit.call(environmentSlot(OBJECT_OFFSETOF(StubEnvironment, stub)), JITThunkPtrTag);

    for (size_t i = 0; i < allocatableGPRs.size(); ++i)
        jit.store64(allocatableGPRs[i], environmentSlot(OBJECT_OFFSETOF(StubEnvironment, registersAfterCall), i));
    if (signature.result == StubResult::GPR)
        jit.store64(signature.firstResult.gpr(), environmentSlot(OBJECT_OFFSETOF(StubEnvironment, result)));
    if (signature.result == StubResult::FPR)
        jit.storeDouble(signature.firstResult.fpr(), environmentSlot(OBJECT_OFFSETOF(StubEnvironment, result)));
    if (signature.result == StubResult::Flags) {
        CCallHelpers::Jump isFalse = jit.branchOnFlags(signature.trueCondition == MacroAssembler::Zero ? CCallHelpers::NotEqual : CCallHelpers::Equal);
        jit.store64(CCallHelpers::TrustedImm32(1), environmentSlot(OBJECT_OFFSETOF(StubEnvironment, result)));
        isFalse.link(&jit);
    }
    for (size_t i = 0; i < markedFPRs.size(); ++i)
        jit.storeDouble(markedFPRs[i], environmentSlot(OBJECT_OFFSETOF(StubEnvironment, numbersAfterCall), i));
}

inline MacroAssemblerCodeRef<JSEntryPtrTag> StubRunner::compileStubCaller(const StubSignature& signature, bool passesImmediate)
{
    Vector<GPRReg> savedRegisters;
    Vector<FPRReg> savedNumbers;
    RegisterSet::vmCalleeSaveRegisters().forEach([&](Reg reg) {
        if (reg.isGPR())
            savedRegisters.append(reg.gpr());
        else
            savedNumbers.append(reg.fpr());
    });
    AOT_TEST_CHECK(savedRegisters.contains(instanceGPR) && savedRegisters.contains(cellCageBaseGPR));
    auto slotOf = [](size_t index) {
        return CCallHelpers::Address(GPRInfo::callFrameRegister, -static_cast<int32_t>((index + 1) * sizeof(CPURegister)));
    };

    CCallHelpers jit;
    jit.emitFunctionPrologue();
    jit.subPtr(CCallHelpers::TrustedImm32(WTF::roundUpToMultipleOf<stackAlignmentBytes()>((savedRegisters.size() + savedNumbers.size()) * sizeof(CPURegister))), CCallHelpers::stackPointerRegister);
    for (size_t i = 0; i < savedRegisters.size(); ++i)
        jit.storePtr(savedRegisters[i], slotOf(i));
    for (size_t i = 0; i < savedNumbers.size(); ++i)
        jit.storeDouble(savedNumbers[i], slotOf(savedRegisters.size() + i));
    jit.move(GPRInfo::argumentGPR0, instanceGPR);
    emitStubCall(jit, signature, passesImmediate);
    for (size_t i = 0; i < savedRegisters.size(); ++i)
        jit.loadPtr(slotOf(i), savedRegisters[i]);
    for (size_t i = 0; i < savedNumbers.size(); ++i)
        jit.loadDouble(slotOf(savedRegisters.size() + i), savedNumbers[i]);
    jit.emitFunctionEpilogue();
    jit.ret();

    LinkBuffer linkBuffer(jit, nullptr);
    return FINALIZE_CODE(linkBuffer, JSEntryPtrTag, nullptr, "AOT stub caller");
}

inline void StubRunner::emitLandingInCompiledFunction(CCallHelpers& jit)
{
    jit.pushToSave(GPRInfo::returnValueGPR);
    jit.load64(environmentSlot(OBJECT_OFFSETOF(StubEnvironment, landedCall.values.callee)), GPRInfo::returnValueGPR);
    jit.store64(GPRInfo::returnValueGPR, environmentSlot(OBJECT_OFFSETOF(StubEnvironment, calleeOfEarlierLanding)));
    jit.load64(environmentSlot(OBJECT_OFFSETOF(StubEnvironment, landedCall.values.thisValue)), GPRInfo::returnValueGPR);
    jit.store64(GPRInfo::returnValueGPR, environmentSlot(OBJECT_OFFSETOF(StubEnvironment, thisValueOfEarlierLanding)));
    jit.popToRestore(GPRInfo::returnValueGPR);
    jit.store64(CCallHelpers::TrustedImm32(static_cast<int32_t>(LandedCall::Place::CompiledFunction)), environmentSlot(OBJECT_OFFSETOF(StubEnvironment, landedCall.place)));
    jit.store64(countGPR, environmentSlot(OBJECT_OFFSETOF(StubEnvironment, landedCall.count)));
    jit.store64(calleeGPR, environmentSlot(OBJECT_OFFSETOF(StubEnvironment, landedCall.calleeInRegister)));
    jit.store64(calleeGPR, environmentSlot(OBJECT_OFFSETOF(StubEnvironment, landedCall.values.callee)));
    jit.store64(thisGPR, environmentSlot(OBJECT_OFFSETOF(StubEnvironment, landedCall.values.thisValue)));
    for (unsigned i = 0; i < numberOfArgumentGPRs; ++i)
        jit.store64(argumentGPR(i), environmentSlot(OBJECT_OFFSETOF(StubEnvironment, landedCall.values.arguments), i));
    jit.move(CCallHelpers::TrustedImm64(wordThatNobodyWrote), calleeGPR);
    jit.move(calleeGPR, thisGPR);
    for (unsigned i = 0; i < numberOfArgumentGPRs; ++i)
        jit.move(calleeGPR, argumentGPR(i));
    jit.load64(environmentSlot(OBJECT_OFFSETOF(StubEnvironment, resultOfNextLanding)), GPRInfo::returnValueGPR);
    jit.store64(CCallHelpers::TrustedImm32(static_cast<int32_t>(resultOfCompiledFunction)), environmentSlot(OBJECT_OFFSETOF(StubEnvironment, resultOfNextLanding)));
    jit.ret();
}

inline void StubRunner::emitLandingInEngineCallee(CCallHelpers& jit)
{
    constexpr GPRReg word = gprOf(StubRegister::T9);
    static_assert(noOverlap(word, BaselineJITRegisters::Call::calleeGPR, instanceGPR));
    auto copySlot = [&](CallFrameSlot slot, unsigned index, size_t offsetInEnvironment) {
        int32_t offset = (static_cast<int32_t>(slot) + static_cast<int32_t>(index)) * static_cast<int32_t>(sizeof(Register)) - static_cast<int32_t>(prologueStackPointerDelta());
        jit.load64(CCallHelpers::Address(CCallHelpers::stackPointerRegister, offset), word);
        jit.store64(word, environmentSlot(offsetInEnvironment, index));
    };

    jit.store64(CCallHelpers::TrustedImm32(static_cast<int32_t>(LandedCall::Place::EngineCallee)), environmentSlot(OBJECT_OFFSETOF(StubEnvironment, landedCall.place)));
    jit.store64(BaselineJITRegisters::Call::calleeGPR, environmentSlot(OBJECT_OFFSETOF(StubEnvironment, landedCall.calleeInRegister)));
    copySlot(CallFrameSlot::argumentCountIncludingThis, 0, OBJECT_OFFSETOF(StubEnvironment, landedCall.count));
    copySlot(CallFrameSlot::callee, 0, OBJECT_OFFSETOF(StubEnvironment, landedCall.values.callee));
    copySlot(CallFrameSlot::thisArgument, 0, OBJECT_OFFSETOF(StubEnvironment, landedCall.values.thisValue));
    for (unsigned i = 0; i < numberOfArgumentGPRs; ++i)
        copySlot(CallFrameSlot::firstArgument, i, OBJECT_OFFSETOF(StubEnvironment, landedCall.values.arguments));
    jit.move(CCallHelpers::TrustedImm64(resultOfEngineCallee), GPRInfo::returnValueGPR);
    jit.ret();
}

inline void StubRunner::emitLandingThatCountsWordsNobodyWrote(CCallHelpers& jit, bool leavesStub)
{
    constexpr GPRReg cursor = GPRInfo::argumentGPR1;
    constexpr GPRReg count = GPRInfo::argumentGPR2;
    constexpr GPRReg word = GPRInfo::argumentGPR3;
    jit.move(CCallHelpers::stackPointerRegister, cursor);
#if CPU(X86_64)
    jit.addPtr(CCallHelpers::TrustedImm32(sizeof(CPURegister)), cursor);
#endif
    jit.move(CCallHelpers::TrustedImm32(0), count);
    CCallHelpers::Label next = jit.label();
    CCallHelpers::Jump done = jit.branchPtr(CCallHelpers::Equal, cursor, GPRInfo::callFrameRegister);
    jit.load64(CCallHelpers::Address(cursor), word);
    jit.addPtr(CCallHelpers::TrustedImm32(sizeof(CPURegister)), cursor);
    jit.branch64(CCallHelpers::NotEqual, word, CCallHelpers::TrustedImm64(wordThatNobodyWrote)).linkTo(next, &jit);
    jit.add64(CCallHelpers::TrustedImm32(1), count);
    jit.jump().linkTo(next, &jit);
    done.link(&jit);
    jit.store64(count, environmentSlot(OBJECT_OFFSETOF(StubEnvironment, wordsNobodyWrote)));
    jit.add32(CCallHelpers::TrustedImm32(1), environmentSlot(OBJECT_OFFSETOF(StubEnvironment, numberOfOperationCalls)));
    jit.move(CCallHelpers::TrustedImm32(0), GPRInfo::returnValueGPR);
    jit.move(CCallHelpers::TrustedImm32(0), GPRInfo::returnValueGPR2);
    if (leavesStub)
        jit.emitFunctionEpilogue();
    jit.ret();
}

inline void StubRunner::compileLandings()
{
    CCallHelpers jit;
    CCallHelpers::Label compiledFunction = jit.label();
    emitLandingInCompiledFunction(jit);
    CCallHelpers::Label engineCallee = jit.label();
    emitLandingInEngineCallee(jit);
    CCallHelpers::Label countsWordsNobodyWrote = jit.label();
    emitLandingThatCountsWordsNobodyWrote(jit, false);
    CCallHelpers::Label countsWordsNobodyWroteAndLeavesStub = jit.label();
    emitLandingThatCountsWordsNobodyWrote(jit, true);

    LinkBuffer linkBuffer(jit, nullptr);
    m_landingThatCountsWordsNobodyWrote = linkBuffer.locationOf<JSEntryPtrTag>(countsWordsNobodyWrote).untaggedPtr();
    m_landingThatCountsWordsNobodyWroteAndLeavesStub = linkBuffer.locationOf<JSEntryPtrTag>(countsWordsNobodyWroteAndLeavesStub).untaggedPtr();
    m_environment->instance.image.code = linkBuffer.locationOf<JSEntryPtrTag>(compiledFunction).untaggedPtr<const uint8_t*>() - distanceOfCompiledFunction;
    m_environment->runtimeTable[indexOf(Entry::VirtualCallTrampoline)] = linkBuffer.locationOf<JSEntryPtrTag>(engineCallee).taggedPtr();
    m_landings = FINALIZE_CODE(linkBuffer, JSEntryPtrTag, nullptr, "AOT stub landings");
}

inline void* StubRunner::addressOf(StubKey key) const
{
    std::optional<uint32_t> offset = m_blob.offsetOf(key);
    AOT_TEST_CHECK(offset.has_value());
    return tagCodePtr<JITThunkPtrTag>(const_cast<uint8_t*>(m_code.start() + *offset));
}

inline StubRunner::StubRunner()
    : StubRunner(std::span<const StubKey> { })
{
}

inline StubRunner::StubRunner(const StubOptions& options)
    : StubRunner(options, { })
{
}

inline StubRunner::StubRunner(std::span<const StubKey> keysWithBoundImmediates)
    : StubRunner(StubOptions { .targetFeatures = CPUFeatureSet::buildTarget() }, keysWithBoundImmediates)
{
}

inline StubRunner::StubRunner(StubPolicy&& policy, std::span<const StubKey> keysWithBoundImmediates)
    : StubRunner(StubOptions { .targetFeatures = CPUFeatureSet::buildTarget() }, keysWithBoundImmediates, WTF::move(policy))
{
}

inline StubRunner::StubRunner(const StubOptions& options, std::span<const StubKey> keysWithBoundImmediates, StubPolicy&& policy)
    : m_catalog(options, WTF::move(policy))
    , m_blob(buildBlob(m_catalog, keysWithBoundImmediates))
    , m_code(m_blob.code.bytes.span())
    , m_environment(makeUniqueWithoutFastMallocCheck<StubEnvironment>())
{
    AOT_TEST_CHECK(m_blob.code.relocations.isEmpty());
    m_environment->runtimeTable.fill(operationPointer(countOperationCall));
    m_environment->resultOfNextLanding = resultOfCompiledFunction;
    Instance& instance = m_environment->instance;
    instance.runtimeTable = m_environment->runtimeTable.data();
    instance.vm = &m_environment->vm.get();
    instance.numberTag = JSValue::NumberTag;
    instance.notCellMask = JSValue::NotCellMask;
    std::ranges::copy(WTF::intHashSecrets, instance.numbers.hashSecrets);
    instance.propertyCaches.absenceFilters = m_environment->absenceFilters.data();
    instance.propertyCaches.missesOfPairs = m_environment->missesOfPairs.data();
    instance.propertyCaches.accessTable.publish(*std::bit_cast<AccessTable*>(roundUpToMultipleOf<AccessTable::alignment>(std::bit_cast<uintptr_t>(m_environment->roomForAccessTable.data()))));
    instance.realm.keysOfPairs = StringKeyCache::noKeysOfPairs();
    instance.prefix().structureIDBase = std::bit_cast<uintptr_t>(m_environment->emptyStructures.data());
    for (SlotHandlerTable table : allSlotHandlerTables) {
        for (Slot::Kind kind : slotKindsWithHandlers)
            instance.slotHandlers()[static_cast<unsigned>(table)][static_cast<unsigned>(kind)] = addressOf(slotHandlerFor(table, kind));
    }
    for (AnswerHandlerTable table : allAnswerHandlerTables) {
        const std::array handlers = answerHandlerKeys(table);
        for (unsigned bits = 0; bits < handlers.size(); ++bits)
            instance.slotHandlers()[numberOfSlotHandlerTables + static_cast<unsigned>(table)][bits] = addressOf(handlers[bits]);
    }
    compileLandings();
}

inline StubRun StubRunner::run(StubKey key, std::initializer_list<uint64_t> operands, std::optional<uint32_t> immediate)
{
    return run(key, m_catalog.summaryOf(key).signature, operands, immediate);
}

inline StubRun StubRunner::runOperationCall(StubKey key, std::initializer_list<uint64_t> arguments)
{
    AOT_TEST_CHECK(isOperationCallHead(key));
    StubSignature signature = m_catalog.summaryOf(key).signature;
    AOT_TEST_CHECK(!signature.numberOfOperands);
    for (unsigned i = 0; i < arguments.size(); ++i)
        signature.addOperand(operationArgumentGPR(i + 1));
    return run(key, signature, arguments, std::nullopt);
}

inline StubRun StubRunner::run(StubKey key, const StubSignature& signature, std::initializer_list<uint64_t> operands, std::optional<uint32_t> immediate)
{
    constexpr auto markers = [] {
        std::array<uint64_t, markedFPRs.size()> bits { };
        for (size_t i = 0; i < bits.size(); ++i)
            bits[i] = std::bit_cast<uint64_t>(1234.5 + static_cast<double>(i));
        return bits;
    }();
    const StubSummary& summary = m_catalog.summaryOf(key);
    StubEnvironment& environment = *m_environment;
    AOT_TEST_CHECK(operands.size() == signature.numberOfOperands && operands.size() <= environment.operands.size());
    AOT_TEST_CHECK(signature.result == StubResult::GPR || signature.result == StubResult::FPR || signature.result == StubResult::Flags || signature.result == StubResult::None);
    AOT_TEST_CHECK(immediate.has_value() == (signature.immediate && !key.immediate()));

    environment.stub = addressOf(key);
    environment.immediate = immediate.value_or(0);
    environment.result = 0;
    environment.numberOfOperationCalls = 0;
    environment.numberOfRecordedCalls = 0;
    environment.recordedArguments = { };
    environment.landedCall = { };
    environment.calleeOfEarlierLanding = 0;
    environment.thisValueOfEarlierLanding = 0;
    environment.wordsNobodyWrote = 0;
    environment.numbers = markers;
    std::ranges::copy(operands, environment.operands.begin());

    MacroAssemblerCodeRef<JSEntryPtrTag> caller = compileStubCaller(signature, immediate.has_value());
    void* callerAddress = untagCFunctionPtr<JSEntryPtrTag>(caller.code().taggedPtr());
    std::optional<uint32_t> instructionsInStubs;
    std::optional<uint32_t> leapsInStubs;
#if AOT_STUB_RUNNER_COUNTS_INSTRUCTIONS
    if (m_countsInstructions) {
        InstructionCounter counter(m_code.start(), m_blob.code.bytes.size());
        InstructionCounter::start();
        std::bit_cast<void (SYSV_ABI *)(Instance*)>(callerAddress)(&environment.instance);
        InstructionCounter::stop();
        instructionsInStubs = InstructionCounter::count();
        leapsInStubs = InstructionCounter::leaps();
    } else
#endif
        std::bit_cast<void (SYSV_ABI *)(Instance*)>(callerAddress)(&environment.instance);

    auto checkIsAnnounced = [&](Reg reg) {
        if (containsRegister(summary.clobbered, reg) || m_landingMayLeaveStub)
            return;
        const StubDescriptor& descriptor = descriptorOf(key.family());
        dataLogLn("the stub ", descriptor.name, ", form ", key.form(), ", count ", key.count(), ", caller frame ", static_cast<unsigned>(key.callerFrame()), " has written ", reg, ", which its summary does not hold");
        AOT_TEST_CHECK(containsRegister(summary.clobbered, reg));
    };
    for (size_t i = 0; i < markedFPRs.size(); ++i) {
        if (!namesRegister(signature, markedFPRs[i]) && environment.numbersAfterCall[i] != environment.numbers[i])
            checkIsAnnounced(markedFPRs[i]);
    }
    RegisterSet changedRegisters;
    for (size_t i = 0; i < allocatableGPRs.size() && !signature.usesCallConvention; ++i) {
        Reg reg = allocatableGPRs[i];
        if (reg == signature.firstResult || reg == signature.secondResult || (signature.immediate && reg == Reg(*signature.immediate)))
            continue;
        uint64_t valueInFrontOfCall = markOfRegister(i);
        if (registerThatHoldsAddressOfStub(signature, immediate.has_value()) == reg.gpr())
            valueInFrontOfCall = std::bit_cast<uintptr_t>(environment.stub);
        for (unsigned operand = 0; operand < signature.numberOfOperands; ++operand) {
            if (signature.operands[operand] == reg)
                valueInFrontOfCall = environment.operands[operand];
        }
        if (environment.registersAfterCall[i] != valueInFrontOfCall)
            changedRegisters.add(reg, IgnoreVectors);
    }
    changedRegisters.forEach(checkIsAnnounced);
    return { environment.result, !!environment.numberOfOperationCalls, environment.numberOfRecordedCalls, environment.recordedArguments, environment.landedCall, instructionsInStubs, changedRegisters, leapsInStubs };
}

inline StubRun StubRunner::runCall(StubKey key, uint64_t callee, uint64_t thisValue, std::initializer_list<uint64_t> arguments, std::optional<uint32_t> immediate)
{
    const StubSignature& signature = m_catalog.summaryOf(key).signature;
    AOT_TEST_CHECK(signature.usesCallConvention && arguments.size() <= numberOfArgumentGPRs);
    CallRegisterValues& call = m_environment->call;
    call.callee = callee;
    call.thisValue = thisValue;
    call.arguments.fill(unsetArgument);
    std::ranges::copy(arguments, call.arguments.begin());
    if (!signature.numberOfOperands)
        return run(key, { }, immediate);
    AOT_TEST_CHECK(signature.numberOfOperands == 1 && signature.operands[0] == Reg(countGPR));
    return run(key, { arguments.size() });
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
