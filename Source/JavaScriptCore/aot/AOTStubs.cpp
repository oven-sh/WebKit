/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubs.h"

#if ENABLE(AOT)

#include "AOTEmitter.h"
#include "AOTImage.h"
#include "AOTInlineCaches.h"
#include "AOTRuntime.h"
#include "AOTThunks.h"
#include "AOTTypedCompactTable.h"
#include "AOTTypedBuiltins.h"
#include "AOTTypedObject.h"
#include "AOTTypedType.h"
#include "BaselineJITRegisters.h"
#include "CodeBlock.h"
#include "FunctionExecutable.h"
#include "GetterSetter.h"
#include "Interpreter.h"
#include "JSArrayIterator.h"
#include "JSBoundFunction.h"
#include "JSGlobalObject.h"
#include "JSMap.h"
#include "JSMapIterator.h"
#include "JSSet.h"
#include "JSSetIterator.h"
#include "JSStringIterator.h"
#include "JSCInlines.h"
#include "JSWeakMap.h"
#include "JSWeakSet.h"
#include "JSWebAssemblyInstance.h"
#include "LinkBuffer.h"
#include "MaxFrameExtentForSlowPathCall.h"
#include "MegamorphicCache.h"
#include "MicrotaskQueue.h"
#include "RegExp.h"
#include "RegExpObject.h"
#include "ThunkGenerators.h"
#include "VM.h"
#include "YarrJIT.h"
#include "YarrMatchingContextHolder.h"
#include <wtf/NeverDestroyed.h>

namespace JSC { namespace AOT {

using Jump = CCallHelpers::Jump;
using Address = CCallHelpers::Address;
using TrustedImm32 = CCallHelpers::TrustedImm32;

#if CPU(ARM64) || CPU(X86_64)

static CCallHelpers::Label* stubLabels()
{
    static NeverDestroyed<std::array<CCallHelpers::Label, numberOfStubs>> labels;
    return labels->data();
}
static Vector<std::pair<CCallHelpers::Call, Stub>>* s_callsBetweenStubs;
static Vector<CCallHelpers::Label>* s_returnsIntoAdapters;
static Vector<CCallHelpers::Label>* s_returnsFromFindingTargets;
#if CPU(ARM64)
struct LabelAddress {
    CCallHelpers::Label instruction;
    GPRReg reg;
    CCallHelpers::Label target;
};
static Vector<LabelAddress>* s_labelAddresses;
#endif
static CCallHelpers::Label& callTargetWithList() { static NeverDestroyed<CCallHelpers::Label> label; return label; }
static CCallHelpers::Label& returnFromCallWithList() { static NeverDestroyed<CCallHelpers::Label> label; return label; }

static void callStubFromStub(CCallHelpers& jit, Stub stub)
{
    s_callsBetweenStubs->append({ jit.nearCall(), stub });
}

static void jumpToEntry(CCallHelpers& jit, GPRReg data, Entry entry)
{
    jit.loadPtr(Address(data, Instance::offsetOfRuntimeTable()), data);
    jit.loadPtr(Address(data, static_cast<unsigned>(entry) * sizeof(void*)), data);
    jit.farJump(data, JITThunkPtrTag);
}

constexpr GPRReg R0 = firstStubOperandGPR;
constexpr GPRReg numberTag = GPRInfo::numberTagRegister;
constexpr GPRReg A0 = GPRInfo::argumentGPR0;
constexpr GPRReg A1 = GPRInfo::argumentGPR1;
constexpr GPRReg A2 = GPRInfo::argumentGPR2;
constexpr GPRReg A3 = GPRInfo::argumentGPR3;
constexpr GPRReg A4 = GPRInfo::argumentGPR4;
constexpr GPRReg A5 = GPRInfo::argumentGPR5;
constexpr GPRReg T9 = stubTemporaryGPRs[0];
constexpr GPRReg T10 = stubTemporaryGPRs[1];
constexpr GPRReg T11 = stubTemporaryGPRs[2];
constexpr GPRReg T12 = stubTemporaryGPRs[3];
constexpr GPRReg T13 = stubTemporaryGPRs[4];
#if CPU(ARM64)
constexpr GPRReg T14 = stubTemporaryGPRs[5];
constexpr GPRReg T15 = stubTemporaryGPRs[6];
constexpr GPRReg entryT12 = T12;
constexpr GPRReg entryT13 = T13;
constexpr GPRReg operationGPR = T11;
constexpr GPRReg assertionScratchGPR = T12;
static_assert(noOverlap(thisGPR, countGPR, calleeGPR, T11, T12, T13, T14, T15) && countGPR == T9 && calleeGPR == T10);
#else
constexpr GPRReg T14 = A3;
constexpr GPRReg T15 = functionIndexGPR;
static_assert(T12 == A0 && T15 == T9);
constexpr GPRReg entryT12 = X86Registers::ecx;
constexpr GPRReg entryT13 = X86Registers::edx;
constexpr GPRReg operationGPR = X86Registers::eax;
constexpr GPRReg assertionScratchGPR = CCallHelpers::s_scratchRegister;
static_assert(noOverlap(thisGPR, countGPR, calleeGPR, T11, T13, A0, A1, A2, A3) && countGPR == T9 && calleeGPR == T10);
static_assert(noOverlap(thisGPR, countGPR, calleeGPR, T11, entryT12, entryT13));
static_assert(noOverlap(operationGPR, T9, A0, A1, A2, A3, A4, A5, seventhOperationArgumentGPR, eighthOperationArgumentGPR));
#endif

static void loadLabelAddress(CCallHelpers& jit, CCallHelpers::Label target, GPRReg dest)
{
#if CPU(ARM64)
    s_labelAddresses->append({ jit.label(), dest, target });
    jit.m_assembler.adr(dest, 0);
#else
    constexpr int size = 7;
    int displacement = static_cast<int>(CCallHelpers::differenceBetween(jit.label(), target)) - size;
    auto& buffer = jit.m_assembler.buffer();
    buffer.putByte(static_cast<int8_t>(0x48 | (static_cast<unsigned>(dest) >> 3) << 2));
    buffer.putByte(static_cast<int8_t>(0x8d));
    buffer.putByte(static_cast<int8_t>((static_cast<unsigned>(dest) & 7) << 3 | 5));
    buffer.putInt(displacement);
#endif
}

static void extractBits(CCallHelpers& jit, GPRReg source, unsigned shift, unsigned width, GPRReg dest)
{
#if CPU(ARM64)
    jit.extractUnsignedBitfield64(source, TrustedImm32(shift), TrustedImm32(width), dest);
#else
    RELEASE_ASSERT(width < 32);
    jit.move(source, dest);
    jit.urshift64(TrustedImm32(shift), dest);
    jit.and64(TrustedImm32((1 << width) - 1), dest);
#endif
}

#if CPU(X86_64)
template<size_t size, typename Functor>
static void preservingRegisters(CCallHelpers& jit, const GPRReg (&saved)[size], const Functor& functor, unsigned bytesPushedSinceEntry = 0)
{
    unsigned bytes = size * sizeof(CPURegister);
    if ((bytes + sizeof(CPURegister) + bytesPushedSinceEntry) % stackAlignmentBytes())
        bytes += sizeof(CPURegister);
    jit.subPtr(TrustedImm32(bytes), CCallHelpers::stackPointerRegister);
    for (unsigned i = 0; i < size; ++i)
        jit.store64(saved[i], Address(CCallHelpers::stackPointerRegister, i * sizeof(CPURegister)));
    functor();
    for (unsigned i = 0; i < size; ++i)
        jit.load64(Address(CCallHelpers::stackPointerRegister, i * sizeof(CPURegister)), saved[i]);
    jit.addPtr(TrustedImm32(bytes), CCallHelpers::stackPointerRegister);
}
#endif

static void loadInstance(CCallHelpers& jit, GPRReg result)
{
    jit.move(instanceGPR, result);
}

static void structureWithID(CCallHelpers& jit, GPRReg idAndResult)
{
    jit.add64(Address(instanceGPR, Instance::offsetOfStructureIDBase()), idAndResult);
}

static void loadSites(CCallHelpers& jit, GPRReg info, GPRReg result)
{
    jit.load32(Address(info, FunctionInfo::offsetOfSites()), result);
    jit.add64(Address(instanceGPR, Instance::offsetOfImage()), result);
}

static unsigned s_codeGranuleShift;
static bool s_functionLengthsAreBytes;

#if CPU(X86_64)
void loadFunctionIndexAt(CCallHelpers& jit, GPRReg pc)
{
    static_assert(functionIndexGPR == T15);
    constexpr GPRReg offset = CCallHelpers::s_scratchRegister;
    ASSERT(pc != T15);
    jit.move(pc, offset);
    jit.sub64(Address(instanceGPR, Instance::offsetOfCode()), offset);
    jit.move(offset, T15);
    jit.urshift64(TrustedImm32(s_codeGranuleShift), T15);
    GPRReg start = pc;
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfCodeGranules()), start);
    jit.load32(CCallHelpers::BaseIndex(start, T15, CCallHelpers::TimesFour), T15);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfSubsequentFunctionStarts()), start);
    jit.x86Lea64(CCallHelpers::BaseIndex(start, T15, CCallHelpers::TimesFour), start);
    CCallHelpers::Label next = jit.label();
    Jump found = jit.branch32(CCallHelpers::Above, Address(start), offset);
    jit.add32(TrustedImm32(1), T15);
    jit.add64(TrustedImm32(sizeof(uint32_t)), start);
    jit.jump().linkTo(next, &jit);
    found.link(&jit);
}

static void loadCallerIndex(CCallHelpers& jit, unsigned bytesPushed = 0, bool callerIsThunk = false, bool keepsTemporary = true)
{
    constexpr GPRReg pc = T13;
    if (keepsTemporary)
        jit.push(pc);
    if (callerIsThunk)
        jit.loadPtr(Address(GPRInfo::callFrameRegister, sizeof(CPURegister)), pc);
    else
        jit.loadPtr(Address(CCallHelpers::stackPointerRegister, bytesPushed + (keepsTemporary ? sizeof(CPURegister) : 0)), pc);
    loadFunctionIndexAt(jit, pc);
    if (keepsTemporary)
        jit.pop(pc);
}
#else
void loadFunctionIndexAt(CCallHelpers& jit, GPRReg pc)
{
    static_assert(functionIndexGPR == T15);
    ASSERT(noOverlap(pc, T14, T15));
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfCode()), T14);
    jit.subPtr(pc, T14, T14);
    jit.urshiftPtr(T14, TrustedImm32(s_codeGranuleShift), T15);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfCodeGranules()), CCallHelpers::memoryTempRegister);
    jit.load32(CCallHelpers::BaseIndex(CCallHelpers::memoryTempRegister, T15, CCallHelpers::TimesFour), T15);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfSubsequentFunctionStarts()), CCallHelpers::memoryTempRegister);
    CCallHelpers::Label next = jit.label();
    jit.load32(CCallHelpers::BaseIndex(CCallHelpers::memoryTempRegister, T15, CCallHelpers::TimesFour), CCallHelpers::dataTempRegister);
    Jump found = jit.branch32(CCallHelpers::Above, CCallHelpers::dataTempRegister, T14);
    jit.add32(TrustedImm32(1), T15);
    jit.jump().linkTo(next, &jit);
    found.link(&jit);
}

static void loadCallerIndex(CCallHelpers& jit, unsigned = 0, bool callerIsThunk = false, bool = true)
{
    if (!callerIsThunk) {
        loadFunctionIndexAt(jit, CCallHelpers::linkRegister);
        return;
    }
    jit.loadPtr(Address(GPRInfo::callFrameRegister, sizeof(CPURegister)), T13);
    loadFunctionIndexAt(jit, T13);
}
#endif

static void loadInfo(CCallHelpers& jit, GPRReg result)
{
    constexpr GPRReg instance = instanceGPR;
    ASSERT(result != T15);
    static_assert(sizeof(FunctionInfo) == 8);
    jit.lshiftPtr(T15, TrustedImm32(3), result);
#if CPU(X86_64)
    jit.add64(Address(instance, Instance::offsetOfInfos()), result);
#else
    jit.loadPtr(Address(instance, Instance::offsetOfInfos()), CCallHelpers::memoryTempRegister);
    jit.addPtr(CCallHelpers::memoryTempRegister, result);
#endif
}

static void loadStateNumber(CCallHelpers& jit, GPRReg instance, GPRReg function, GPRReg result)
{
    static_assert(sizeof(FunctionInfo) == 8);
    jit.lshiftPtr(function, TrustedImm32(3), result);
    jit.addPtr(Address(instance, Instance::offsetOfInfos()), result);
    jit.load32(Address(result, FunctionInfo::offsetOfBitsAndStateNumber()), result);
    jit.urshift32(TrustedImm32(FunctionInfo::stateNumberShift), result);
}

static void loadDataForSlot(CCallHelpers& jit, GPRReg slot, GPRReg data)
{
    constexpr GPRReg instance = instanceGPR;
    ASSERT(noOverlap(slot, data, T15));
    jit.loadPtr(Address(instance, Instance::offsetOfSharedData()), data);
#if CPU(X86_64)
    static_assert(SharedData::size <= std::numeric_limits<int32_t>::max());
    jit.addPtr(TrustedImm32(BiasedDataPointer::bias), slot, CCallHelpers::s_scratchRegister);
    jit.sub64(data, CCallHelpers::s_scratchRegister);
    Jump isShared = jit.branch64(CCallHelpers::Below, CCallHelpers::s_scratchRegister, TrustedImm32(SharedData::size));
#else
    jit.subPtr(slot, data, CCallHelpers::memoryTempRegister);
    jit.addPtr(TrustedImm32(BiasedDataPointer::bias), CCallHelpers::memoryTempRegister);
    jit.urshiftPtr(TrustedImm32(12), CCallHelpers::memoryTempRegister);
    Jump isShared = jit.branchPtr(CCallHelpers::Below, CCallHelpers::memoryTempRegister, CCallHelpers::TrustedImmPtr((SharedData::size + 4095) >> 12));
#endif
#if CPU(ARM64)
    static_assert(sizeof(FunctionInfo) == 8);
    jit.loadPtr(Address(instance, Instance::offsetOfInfos()), data);
    jit.addLeftShift64(data, T15, TrustedImm32(3), data);
    jit.load32(Address(data, FunctionInfo::offsetOfBitsAndStateNumber()), data);
    jit.urshift32(TrustedImm32(FunctionInfo::stateNumberShift), data);
    jit.loadPtr(Address(instance, Instance::offsetOfAddressOfStates()), CCallHelpers::memoryTempRegister);
    jit.load16(CCallHelpers::BaseIndex(CCallHelpers::memoryTempRegister, data, CCallHelpers::TimesTwo), data);
#else
    loadStateNumber(jit, instance, T15, data);
    jit.load16(CCallHelpers::BaseIndex(instance, data, CCallHelpers::TimesTwo, Instance::offsetOfStates()), data);
#endif
    jit.loadPtr(CCallHelpers::BaseIndex(instance, data, CCallHelpers::TimesEight), data);
    isShared.link(&jit);
}

// `keepsAllRegisters`: for a stub whose caller counts on every register but the temporaries.
static void countSlotMiss(CCallHelpers& jit, GPRReg data, bool keepsAllRegisters = false)
{
    UNUSED_PARAM(keepsAllRegisters);
    constexpr GPRReg instance = instanceGPR;
    ASSERT(data == T10);
    Jump hasOwnData = jit.branchPtr(CCallHelpers::NotEqual, Address(instance, Instance::offsetOfSharedData()), data);
    static_assert(sizeof(FunctionInfo) == 8);
#if CPU(X86_64)
    constexpr GPRReg function = T15;
    CCallHelpers::BaseIndex counter(instance, T12, CCallHelpers::TimesTwo, Instance::offsetOfMissCounters());
    jit.and32(TrustedImm32(Instance::numberOfMissCounters - 1), function, T12);
    jit.load16(counter, T13);
    jit.add32(TrustedImm32(1), T13);
    jit.store16(T13, counter);
    jit.lshiftPtr(function, TrustedImm32(3), T12);
#else
    constexpr GPRReg function = T11;
    jit.move(T15, T11);
    jit.and32(TrustedImm32(Instance::numberOfMissCounters - 1), T11, T13);
    jit.addPtr(TrustedImm32(Instance::offsetOfMissCounters()), instance, T12);
    jit.getEffectiveAddress(CCallHelpers::BaseIndex(T12, T13, CCallHelpers::TimesTwo), T12);
    jit.load16(Address(T12), T13);
    jit.add32(TrustedImm32(1), T13);
    jit.store16(T13, Address(T12));
    jit.lshiftPtr(T11, TrustedImm32(3), T12);
#endif
    UNUSED_VARIABLE(function);
#if CPU(X86_64)
    jit.add64(Address(instance, Instance::offsetOfInfos()), T12);
    loadSites(jit, T12, T12);
    jit.load16(Address(T12, ImageFunction::offsetOfNumSlotsFromSites()), T12);
    jit.add32(TrustedImm32(Instance::missesBeyondItsSlots), T12);
    Jump notReachedYet = jit.branch32(CCallHelpers::Below, T13, T12);
    {
        constexpr GPRReg saved[] = { R0, A1, A2, A3, T10, T15 };
        preservingRegisters(jit, saved, [&] {
            jit.move(instance, A0);
            jit.move(function, A1);
            jit.loadPtr(Address(instance, Instance::offsetOfRuntimeTable()), T13);
            jit.loadPtr(Address(T13, static_cast<unsigned>(Entry::operationAOTEnsureData) * sizeof(void*)), T13);
            jit.call(T13, OperationPtrTag);
        });
    }
#else
    jit.loadPtr(Address(instance, Instance::offsetOfInfos()), CCallHelpers::memoryTempRegister);
    jit.addPtr(CCallHelpers::memoryTempRegister, T12);
    loadSites(jit, T12, T12);
    jit.load16(Address(T12, ImageFunction::offsetOfNumSlotsFromSites()), T12);
    jit.add32(TrustedImm32(Instance::missesBeyondItsSlots), T12);
    Jump notReachedYet = jit.branch32(CCallHelpers::Below, T13, T12);
    auto fpr = [](unsigned i) { return static_cast<FPRReg>(i < 8 ? ARM64Registers::q0 + i : ARM64Registers::q16 + (i - 8)); };
    const int32_t room = 96 + (keepsAllRegisters ? 16 + 24 * 8 : 0);
    jit.subPtr(TrustedImm32(room), CCallHelpers::stackPointerRegister);
    if (keepsAllRegisters) {
        jit.storePair64(GPRInfo::argumentGPR7, ARM64Registers::x8, CCallHelpers::stackPointerRegister, TrustedImm32(96));
        for (unsigned i = 0; i < 24; i += 2)
            jit.storePairDouble(fpr(i), fpr(i + 1), Address(CCallHelpers::stackPointerRegister, 112 + i * 8));
    }
    jit.storePair64(R0, A1, CCallHelpers::stackPointerRegister, TrustedImm32(0));
    jit.storePair64(A2, A3, CCallHelpers::stackPointerRegister, TrustedImm32(16));
    jit.storePair64(A4, A5, CCallHelpers::stackPointerRegister, TrustedImm32(32));
    jit.storePair64(GPRInfo::argumentGPR6, CCallHelpers::linkRegister, CCallHelpers::stackPointerRegister, TrustedImm32(48));
    jit.storePair64(T9, data, CCallHelpers::stackPointerRegister, TrustedImm32(64));
    jit.storePtr(T15, Address(CCallHelpers::stackPointerRegister, 80));
    jit.move(instance, A0);
    jit.move(T11, A1);
    jit.loadPtr(Address(instance, Instance::offsetOfRuntimeTable()), T12);
    jit.loadPtr(Address(T12, static_cast<unsigned>(Entry::operationAOTEnsureData) * sizeof(void*)), T12);
    jit.call(T12, OperationPtrTag);
    jit.loadPair64(CCallHelpers::stackPointerRegister, TrustedImm32(0), R0, A1);
    jit.loadPair64(CCallHelpers::stackPointerRegister, TrustedImm32(16), A2, A3);
    jit.loadPair64(CCallHelpers::stackPointerRegister, TrustedImm32(32), A4, A5);
    jit.loadPair64(CCallHelpers::stackPointerRegister, TrustedImm32(48), GPRInfo::argumentGPR6, CCallHelpers::linkRegister);
    jit.loadPair64(CCallHelpers::stackPointerRegister, TrustedImm32(64), T9, data);
    jit.loadPtr(Address(CCallHelpers::stackPointerRegister, 80), T15);
    if (keepsAllRegisters) {
        jit.loadPair64(CCallHelpers::stackPointerRegister, TrustedImm32(96), GPRInfo::argumentGPR7, ARM64Registers::x8);
        for (unsigned i = 0; i < 24; i += 2)
            jit.loadPairDouble(Address(CCallHelpers::stackPointerRegister, 112 + i * 8), fpr(i), fpr(i + 1));
    }
    jit.addPtr(TrustedImm32(room), CCallHelpers::stackPointerRegister);
#endif
    notReachedYet.link(&jit);
    hasOwnData.link(&jit);
}

enum class Returns : uint8_t { Value, Void, Double };

static void prepareCallOperationWithVM(CCallHelpers& jit, GPRReg vm)
{
#if ASSERT_ENABLED
    jit.storePtr(GPRInfo::callFrameRegister, Address(vm, VM::offsetOfTopCallFrame()));
#else
    UNUSED_PARAM(jit);
    UNUSED_PARAM(vm);
#endif
}

static void prepareCallOperation(CCallHelpers& jit, GPRReg scratch)
{
#if ASSERT_ENABLED
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfVM()), scratch);
#endif
    prepareCallOperationWithVM(jit, scratch);
}

static void callAndCheckException(CCallHelpers& jit, GPRReg function, Returns returns, GPRReg result = GPRInfo::returnValueGPR, bool callerIsThunk = false, bool checksException = true)
{
    if (callerIsThunk) {
#if CPU(ARM64)
        jit.pushToSave(CCallHelpers::linkRegister);
#else
        jit.subPtr(TrustedImm32(sizeof(CPURegister)), CCallHelpers::stackPointerRegister);
#endif
    } else
        jit.emitFunctionPrologue();
#if CPU(X86_64)
    jit.push(eighthOperationArgumentGPR);
    jit.push(seventhOperationArgumentGPR);
#endif
    ASSERT(function != assertionScratchGPR);
    prepareCallOperation(jit, assertionScratchGPR);
    jit.call(function, OperationPtrTag);
    if (callerIsThunk) {
#if CPU(ARM64)
        jit.popToRestore(CCallHelpers::linkRegister);
#else
        jit.addPtr(TrustedImm32(3 * sizeof(CPURegister)), CCallHelpers::stackPointerRegister);
#endif
    } else
        jit.emitFunctionEpilogue();
    if (!checksException) {
        if (returns == Returns::Double) {
            loadInstance(jit, T9);
            jit.loadPtr(Address(T9, Instance::offsetOfVM()), T9);
            jit.loadPtr(Address(T9, VM::exceptionOffset()), GPRInfo::returnValueGPR);
        }
        jit.ret();
        return;
    }
    Jump exception;
    switch (returns) {
    case Returns::Value:
        exception = jit.branchTestPtr(CCallHelpers::NonZero, GPRInfo::returnValueGPR2);
        break;
    case Returns::Void:
        exception = jit.branchTestPtr(CCallHelpers::NonZero, GPRInfo::returnValueGPR);
        break;
    case Returns::Double:
        loadInstance(jit, T9);
        jit.loadPtr(Address(T9, Instance::offsetOfVM()), T9);
        exception = jit.branchTestPtr(CCallHelpers::NonZero, Address(T9, VM::exceptionOffset()));
        break;
    }
    jit.move(GPRInfo::returnValueGPR, result);
    jit.ret();
    exception.link(&jit);
    if (callerIsThunk)
        jit.emitFunctionEpilogue();
    loadInstance(jit, T9);
    jumpToEntry(jit, T9, Entry::HandleException);
}

enum class Supplies : uint8_t { Nothing, GlobalObject, Instance };
static void generateOperation(CCallHelpers& jit, Returns returns, Supplies supplies, GPRReg result = GPRInfo::returnValueGPR, bool checksException = true)
{
    loadInstance(jit, operationGPR);
    if (supplies == Supplies::GlobalObject)
        jit.loadPtr(Address(operationGPR, Instance::offsetOfGlobalObject()), A0);
    else if (supplies == Supplies::Instance)
        jit.move(operationGPR, A0);
    jit.loadPtr(Address(operationGPR, Instance::offsetOfRuntimeTable()), operationGPR);
    jit.loadPtr(CCallHelpers::BaseIndex(operationGPR, T9, CCallHelpers::TimesOne), operationGPR);
    callAndCheckException(jit, operationGPR, returns, result, false, checksException);
}

static GPRReg numberOfColdOperationGPR()
{
#if CPU(X86_64)
    return CCallHelpers::s_scratchRegister;
#else
    return T9;
#endif
}

#if CPU(X86_64)
static void generateColdOperation(CCallHelpers& jit, bool isLeaf, bool returnsValue = false, bool checksException = true, bool suppliesInstance = true, bool returnsDouble = false, bool savesFPRs = true, GPRReg number = T9)
{
    RELEASE_ASSERT(!isLeaf);
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    constexpr GPRReg scratch = CCallHelpers::s_scratchRegister;
    constexpr GPRReg saved[] = { X86Registers::eax, X86Registers::ecx, X86Registers::edx, X86Registers::esi, X86Registers::edi, X86Registers::r8, X86Registers::r9, X86Registers::r10 };
    constexpr unsigned numberOfGPRs = std::size(saved);
    static_assert(!(numberOfGPRs % 2));
    const unsigned numberOfFPRs = savesFPRs ? 16 : 0;
    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32((numberOfGPRs + numberOfFPRs) * 8), sp);
    for (unsigned i = 0; i < numberOfGPRs; ++i)
        jit.store64(saved[i], Address(sp, i * 8));
    for (unsigned i = 0; i < numberOfFPRs; ++i)
        jit.storeDouble(static_cast<FPRReg>(X86Registers::xmm0 + i), Address(sp, (numberOfGPRs + i) * 8));
    if (suppliesInstance)
        jit.move(instanceGPR, A0);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), operationGPR);
    jit.loadPtr(CCallHelpers::BaseIndex(operationGPR, number, CCallHelpers::TimesOne), operationGPR);
    prepareCallOperation(jit, assertionScratchGPR);
    jit.call(operationGPR, OperationPtrTag);
    if (returnsDouble) {
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfVM()), scratch);
        jit.loadPtr(Address(scratch, VM::exceptionOffset()), scratch);
    } else
        jit.move(returnsValue ? GPRInfo::returnValueGPR2 : GPRInfo::returnValueGPR, scratch);
    for (unsigned i = 0; i < numberOfGPRs; ++i) {
        if (!returnsValue || saved[i] != GPRInfo::returnValueGPR)
            jit.load64(Address(sp, i * 8), saved[i]);
    }
    static_assert(FPRInfo::returnValueFPR == X86Registers::xmm0);
    for (unsigned i = returnsDouble; i < numberOfFPRs; ++i)
        jit.loadDouble(Address(sp, (numberOfGPRs + i) * 8), static_cast<FPRReg>(X86Registers::xmm0 + i));
    jit.emitFunctionEpilogue();
    if (!checksException) {
        jit.ret();
        return;
    }
    Jump exception = jit.branchTestPtr(CCallHelpers::NonZero, scratch);
    jit.ret();
    exception.link(&jit);
    loadInstance(jit, T9);
    jumpToEntry(jit, T9, Entry::HandleException);
}
template<typename Functor>
static void generateColdOperationPlacingArguments(CCallHelpers& jit, Entry operation, unsigned numberOfValues, const Functor& placeArguments)
{
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    constexpr GPRReg scratch = CCallHelpers::s_scratchRegister;
    constexpr GPRReg saved[] = { X86Registers::ecx, X86Registers::edx, X86Registers::esi, X86Registers::edi, X86Registers::r8, X86Registers::r9, X86Registers::r10 };
    constexpr unsigned numberOfGPRs = std::size(saved);
    constexpr unsigned numberOfFPRs = 16;
    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(WTF::roundUpToMultipleOf<stackAlignmentBytes()>((numberOfValues + numberOfGPRs + numberOfFPRs) * sizeof(CPURegister))), sp);
    for (unsigned i = 0; i < numberOfGPRs; ++i)
        jit.store64(saved[i], Address(sp, (numberOfValues + i) * sizeof(CPURegister)));
    for (unsigned i = 0; i < numberOfFPRs; ++i)
        jit.storeDouble(static_cast<FPRReg>(X86Registers::xmm0 + i), Address(sp, (numberOfValues + numberOfGPRs + i) * sizeof(CPURegister)));
    placeArguments();
    jit.move(instanceGPR, A0);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), operationGPR);
    jit.loadPtr(Address(operationGPR, static_cast<unsigned>(operation) * sizeof(void*)), operationGPR);
    prepareCallOperation(jit, assertionScratchGPR);
    jit.call(operationGPR, OperationPtrTag);
    jit.move(GPRInfo::returnValueGPR2, scratch);
    for (unsigned i = 0; i < numberOfGPRs; ++i)
        jit.load64(Address(sp, (numberOfValues + i) * sizeof(CPURegister)), saved[i]);
    for (unsigned i = 0; i < numberOfFPRs; ++i)
        jit.loadDouble(Address(sp, (numberOfValues + numberOfGPRs + i) * sizeof(CPURegister)), static_cast<FPRReg>(X86Registers::xmm0 + i));
    jit.emitFunctionEpilogue();
    Jump exception = jit.branchTestPtr(CCallHelpers::NonZero, scratch);
    jit.ret();
    exception.link(&jit);
    loadInstance(jit, T9);
    jumpToEntry(jit, T9, Entry::HandleException);
}
static void generateLeafColdOperationVoid(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafColdOperationValue(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafColdPlainOperation(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafColdOperationVoidWithoutFPRs(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafColdOperationValueWithoutFPRs(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafColdPlainOperationWithoutFPRs(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafColdPlainOperationWithoutInstance(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafColdPlainOperationDouble(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafColdPlainOperationDoubleWithoutInstance(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafColdOperationDouble(CCallHelpers& jit) { jit.breakpoint(); }
// Behind the call of this stub stands a jump to a function of the program that takes nothing and yields nothing. Every register is kept.
static void generateColdCallFunction(CCallHelpers& jit)
{
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    constexpr GPRReg saved[] = { X86Registers::eax, X86Registers::ecx, X86Registers::edx, X86Registers::esi, X86Registers::edi, X86Registers::r8, X86Registers::r9, X86Registers::r10, X86Registers::r12, X86Registers::ebx };
    constexpr unsigned numberOfGPRs = std::size(saved);
    static_assert(!(numberOfGPRs % 2));
    constexpr unsigned numberOfFPRs = 16;
    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32((numberOfGPRs + numberOfFPRs) * 8), sp);
    for (unsigned i = 0; i < numberOfGPRs; ++i)
        jit.store64(saved[i], Address(sp, i * 8));
    for (unsigned i = 0; i < numberOfFPRs; ++i)
        jit.storeDouble(static_cast<FPRReg>(X86Registers::xmm0 + i), Address(sp, (numberOfGPRs + i) * 8));
    jit.loadPtr(Address(GPRInfo::callFrameRegister, sizeof(void*)), CCallHelpers::s_scratchRegister);
    jit.call(CCallHelpers::s_scratchRegister, JSEntryPtrTag);
    for (unsigned i = 0; i < numberOfGPRs; ++i)
        jit.load64(Address(sp, i * 8), saved[i]);
    for (unsigned i = 0; i < numberOfFPRs; ++i)
        jit.loadDouble(Address(sp, (numberOfGPRs + i) * 8), static_cast<FPRReg>(X86Registers::xmm0 + i));
    jit.emitFunctionEpilogue();
    jit.addPtr(TrustedImm32(sizeOfNearCall), Address(sp));
    jit.ret();
}
static void generateLeafColdCallFunction(CCallHelpers& jit) { jit.breakpoint(); }
#else
static void generateColdOperation(CCallHelpers& jit, bool isLeaf, bool returnsValue = false, bool checksException = true, bool suppliesInstance = true, bool returnsDouble = false, bool savesFPRs = true, std::optional<unsigned> numberOfValuesInArguments = std::nullopt, GPRReg firstArgumentIsIn = InvalidGPRReg, GPRReg secondArgumentIsIn = InvalidGPRReg)
{
    constexpr GPRReg fp = GPRInfo::callFrameRegister;
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    constexpr GPRReg scratch = ARM64Registers::x16;
    if (isLeaf) {
        jit.pushPair(fp, T10);
        jit.move(sp, fp);
    }
    jit.emitFunctionPrologue();
    constexpr unsigned numberOfGPRs = 16;
    const unsigned numberOfFPRs = savesFPRs ? 24 : 0;
    auto fpr = [](unsigned i) { return static_cast<FPRReg>(i < 8 ? ARM64Registers::q0 + i : ARM64Registers::q16 + (i - 8)); };
    jit.subPtr(TrustedImm32((numberOfGPRs + numberOfFPRs) * 8), sp);
    for (unsigned i = 0; i < numberOfGPRs; i += 2)
        jit.storePair64(static_cast<GPRReg>(ARM64Registers::x0 + i), static_cast<GPRReg>(ARM64Registers::x0 + i + 1), sp, TrustedImm32(i * 8));
    for (unsigned i = 0; i < numberOfFPRs; i += 2)
        jit.storePairDouble(fpr(i), fpr(i + 1), Address(sp, (numberOfGPRs + i) * 8));
    // The operation takes how many there are and where: from the second argument register on, as they were saved.
    if (numberOfValuesInArguments) {
        jit.move(TrustedImm32(*numberOfValuesInArguments), A1);
        jit.addPtr(TrustedImm32(8), sp, A2);
    }
    if (firstArgumentIsIn != InvalidGPRReg)
        jit.move(firstArgumentIsIn, A1);
    if (secondArgumentIsIn != InvalidGPRReg)
        jit.move(secondArgumentIsIn, A2);
    loadInstance(jit, T11);
    if (suppliesInstance)
        jit.move(T11, A0);
    jit.loadPtr(Address(T11, Instance::offsetOfRuntimeTable()), T11);
    jit.loadPtr(CCallHelpers::BaseIndex(T11, T9, CCallHelpers::TimesOne), T11);
    prepareCallOperation(jit, T12);
    jit.call(T11, OperationPtrTag);
    if (returnsDouble) {
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfVM()), scratch);
        jit.loadPtr(Address(scratch, VM::exceptionOffset()), scratch);
    } else
        jit.move(returnsValue ? GPRInfo::returnValueGPR2 : GPRInfo::returnValueGPR, scratch);
    if (returnsValue)
        jit.load64(Address(sp, 8), ARM64Registers::x1);
    for (unsigned i = returnsValue ? 2 : 0; i < numberOfGPRs; i += 2)
        jit.loadPair64(sp, TrustedImm32(i * 8), static_cast<GPRReg>(ARM64Registers::x0 + i), static_cast<GPRReg>(ARM64Registers::x0 + i + 1));
    static_assert(FPRInfo::returnValueFPR == ARM64Registers::q0);
    RELEASE_ASSERT(savesFPRs || !returnsDouble);
    if (returnsDouble)
        jit.loadDouble(Address(sp, (numberOfGPRs + 1) * 8), fpr(1));
    for (unsigned i = returnsDouble ? 2 : 0; i < numberOfFPRs; i += 2)
        jit.loadPairDouble(Address(sp, (numberOfGPRs + i) * 8), fpr(i), fpr(i + 1));
    jit.emitFunctionEpilogue();
    Jump exception;
    if (checksException)
        exception = jit.branchTestPtr(CCallHelpers::NonZero, scratch);
    if (isLeaf) {
        jit.move(CCallHelpers::linkRegister, scratch);
        jit.popPair(fp, CCallHelpers::linkRegister);
        jit.m_assembler.ret(scratch);
    } else
        jit.ret();
    if (!checksException)
        return;
    exception.link(&jit);
    loadInstance(jit, T9);
    jumpToEntry(jit, T9, Entry::HandleException);
}
static void generateLeafColdOperationVoid(CCallHelpers& jit) { generateColdOperation(jit, true); }
static void generateLeafColdOperationValue(CCallHelpers& jit) { generateColdOperation(jit, true, true); }
static void generateLeafColdPlainOperation(CCallHelpers& jit) { generateColdOperation(jit, true, true, false); }
static void generateLeafColdOperationVoidWithoutFPRs(CCallHelpers& jit) { generateColdOperation(jit, true, false, true, true, false, false); }
static void generateLeafColdOperationValueWithoutFPRs(CCallHelpers& jit) { generateColdOperation(jit, true, true, true, true, false, false); }
static void generateLeafColdPlainOperationWithoutFPRs(CCallHelpers& jit) { generateColdOperation(jit, true, true, false, true, false, false); }
static void generateLeafColdPlainOperationWithoutInstance(CCallHelpers& jit) { generateColdOperation(jit, true, true, false, false); }
static void generateLeafColdPlainOperationDouble(CCallHelpers& jit) { generateColdOperation(jit, true, false, false, true, true); }
static void generateLeafColdPlainOperationDoubleWithoutInstance(CCallHelpers& jit) { generateColdOperation(jit, true, false, false, false, true); }
static void generateLeafColdOperationDouble(CCallHelpers& jit) { generateColdOperation(jit, true, false, true, true, true); }
// Behind the call of this stub stands a jump to a function of the program that takes nothing and yields nothing. Every register is kept.
static void generateColdCallFunction(CCallHelpers& jit, bool isLeaf)
{
    constexpr GPRReg fp = GPRInfo::callFrameRegister;
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    constexpr GPRReg scratch = ARM64Registers::x16;
    if (isLeaf) {
        jit.pushPair(fp, T10);
        jit.move(sp, fp);
    }
    jit.emitFunctionPrologue();
    constexpr unsigned numberOfGPRs = 16;
    constexpr unsigned numberOfFPRs = 24;
    auto fpr = [](unsigned i) { return static_cast<FPRReg>(i < 8 ? ARM64Registers::q0 + i : ARM64Registers::q16 + (i - 8)); };
    jit.subPtr(TrustedImm32((numberOfGPRs + numberOfFPRs) * 8), sp);
    for (unsigned i = 0; i < numberOfGPRs; i += 2)
        jit.storePair64(static_cast<GPRReg>(ARM64Registers::x0 + i), static_cast<GPRReg>(ARM64Registers::x0 + i + 1), sp, TrustedImm32(i * 8));
    for (unsigned i = 0; i < numberOfFPRs; i += 2)
        jit.storePairDouble(fpr(i), fpr(i + 1), Address(sp, (numberOfGPRs + i) * 8));
    jit.call(CCallHelpers::linkRegister, JSEntryPtrTag);
    for (unsigned i = 0; i < numberOfGPRs; i += 2)
        jit.loadPair64(sp, TrustedImm32(i * 8), static_cast<GPRReg>(ARM64Registers::x0 + i), static_cast<GPRReg>(ARM64Registers::x0 + i + 1));
    for (unsigned i = 0; i < numberOfFPRs; i += 2)
        jit.loadPairDouble(Address(sp, (numberOfGPRs + i) * 8), fpr(i), fpr(i + 1));
    jit.emitFunctionEpilogue();
    jit.addPtr(TrustedImm32(sizeOfNearCall), CCallHelpers::linkRegister, scratch);
    if (isLeaf)
        jit.popPair(fp, CCallHelpers::linkRegister);
    jit.m_assembler.ret(scratch);
}
static void generateColdCallFunction(CCallHelpers& jit) { generateColdCallFunction(jit, false); }
static void generateLeafColdCallFunction(CCallHelpers& jit) { generateColdCallFunction(jit, true); }
#endif
// The return address is that of the word that says what to throw: see Typed::Lowering::throwAt(). The value may be in any register.
// isFault: not called: the handler of prepareFaultingAccesses() has sent the thread here from an access, with every register the compiler allocates as it was.
static void generateThrowAtSite(CCallHelpers& jit, bool isLeaf, bool isFault = false, bool isTrap = false, bool isByWord = false)
{
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    constexpr unsigned numberOfRegistersPerBank = 32;
#if CPU(ARM64)
    if (isLeaf) {
        jit.pushPair(GPRInfo::callFrameRegister, isFault ? ARM64Registers::x16 : T10);
        jit.move(sp, GPRInfo::callFrameRegister);
    }
    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(2 * numberOfRegistersPerBank * 8), sp);
    for (unsigned i = 0; i < 30; i += 2)
        jit.storePair64(static_cast<GPRReg>(ARM64Registers::x0 + i), static_cast<GPRReg>(ARM64Registers::x0 + i + 1), sp, TrustedImm32(i * 8));
    for (unsigned i = 0; i < 32; i += 2)
        jit.storePairDouble(static_cast<FPRReg>(ARM64Registers::q0 + i), static_cast<FPRReg>(ARM64Registers::q0 + i + 1), Address(sp, (numberOfRegistersPerBank + i) * 8));
    jit.move(CCallHelpers::linkRegister, GPRInfo::argumentGPR2);
#else
    RELEASE_ASSERT(!isLeaf || isFault);
    if (isLeaf)
        jit.emitFunctionPrologue();
    if (isFault)
        jit.push(X86Registers::r11);
    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(2 * numberOfRegistersPerBank * 8), sp);
    for (unsigned i = 0; i < 16; ++i)
        jit.store64(static_cast<GPRReg>(X86Registers::eax + i), Address(sp, i * 8));
    for (unsigned i = 0; i < 16; ++i)
        jit.storeDouble(static_cast<FPRReg>(X86Registers::xmm0 + i), Address(sp, (numberOfRegistersPerBank + i) * 8));
    jit.loadPtr(Address(GPRInfo::callFrameRegister, sizeof(void*)), GPRInfo::argumentGPR2);
#endif
    jit.move(sp, A1);
    jit.move(instanceGPR, A0);
    jit.move(TrustedImm32(isByWord ? ThrowSite::wordIsInRegister : isTrap), A3);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T9);
    jit.loadPtr(Address(T9, static_cast<unsigned>(Entry::operationAOTTypedThrowAtSite) * sizeof(void*)), T9);
    prepareCallOperation(jit, T12);
    jit.call(T9, OperationPtrTag);
    if (!isLeaf && !isFault && !isByWord) {
        Address returnAddress(GPRInfo::callFrameRegister, sizeof(void*));
        jit.loadPtr(returnAddress, T9);
        jit.load32(Address(T9), T10);
        Jump unwinds = jit.branch32(CCallHelpers::GreaterThanOrEqual, T10, TrustedImm32(0));
        jit.addPtr(TrustedImm32(sizeof(uint32_t)), T9);
        Jump hasNarrowA = jit.branchTest32(CCallHelpers::Zero, T10, TrustedImm32(ThrowSite::hasWideA));
        jit.addPtr(TrustedImm32(sizeof(uint32_t)), T9);
        hasNarrowA.link(&jit);
        Jump hasNoB = jit.branchTest32(CCallHelpers::Zero, T10, TrustedImm32(ThrowSite::hasB));
        jit.addPtr(TrustedImm32(sizeof(uint32_t)), T9);
        hasNoB.link(&jit);
        jit.storePtr(T9, returnAddress);
#if CPU(ARM64)
        for (unsigned i = 0; i < 30; i += 2) {
            if (i == 18)
                jit.load64(Address(sp, 19 * 8), ARM64Registers::x19);
            else
                jit.loadPair64(sp, TrustedImm32(i * 8), static_cast<GPRReg>(ARM64Registers::x0 + i), static_cast<GPRReg>(ARM64Registers::x0 + i + 1));
        }
        for (unsigned i = 0; i < 32; i += 2)
            jit.loadPairDouble(Address(sp, (numberOfRegistersPerBank + i) * 8), static_cast<FPRReg>(ARM64Registers::q0 + i), static_cast<FPRReg>(ARM64Registers::q0 + i + 1));
#else
        for (unsigned i = 0; i < 16; ++i) {
            GPRReg reg = static_cast<GPRReg>(X86Registers::eax + i);
            if (reg != X86Registers::esp && reg != X86Registers::ebp)
                jit.load64(Address(sp, i * 8), reg);
        }
        for (unsigned i = 0; i < 16; ++i)
            jit.loadDouble(Address(sp, (numberOfRegistersPerBank + i) * 8), static_cast<FPRReg>(X86Registers::xmm0 + i));
#endif
        jit.emitFunctionEpilogue();
        jit.ret();
        unwinds.link(&jit);
    }
    jit.emitFunctionEpilogue();
    loadInstance(jit, T9);
    jumpToEntry(jit, T9, Entry::HandleException);
}
static void generateThrowAtSite(CCallHelpers& jit) { generateThrowAtSite(jit, false); }
static void generateThrowAtSiteByWord(CCallHelpers& jit) { generateThrowAtSite(jit, false, false, false, true); }
static void generateThrowAtFault(CCallHelpers& jit) { generateThrowAtSite(jit, false, true); }
#if CPU(X86_64)
static void generateThrowAtTrap(CCallHelpers& jit) { generateThrowAtSite(jit, false, true, true); }
#else
static void generateThrowAtTrap(CCallHelpers& jit) { jit.breakpoint(); }
#endif
static void generateLeafThrowAtTrap(CCallHelpers& jit) { jit.breakpoint(); }
#if CPU(ARM64)
static void generateLeafThrowAtSite(CCallHelpers& jit) { generateThrowAtSite(jit, true); }
static void generateLeafThrowAtSiteByWord(CCallHelpers& jit) { generateThrowAtSite(jit, true, false, false, true); }
static void generateLeafThrowAtFault(CCallHelpers& jit) { generateThrowAtSite(jit, true, true); }
#else
static void generateLeafThrowAtSite(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafThrowAtSiteByWord(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafThrowAtFault(CCallHelpers& jit)
{
    if (isX86_64())
        generateThrowAtSite(jit, true, true);
    else
        jit.breakpoint();
}
#endif

static void generateColdOperationBehindThunk(CCallHelpers& jit, bool returnsValue = false, bool checksException = true, bool suppliesInstance = true, bool returnsDouble = false, bool savesFPRs = true)
{
#if CPU(X86_64)
    generateColdOperation(jit, false, returnsValue, checksException, suppliesInstance, returnsDouble, savesFPRs, numberOfColdOperationGPR());
#else
    generateColdOperation(jit, false, returnsValue, checksException, suppliesInstance, returnsDouble, savesFPRs);
#endif
}
static void generateColdOperationVoid(CCallHelpers& jit) { generateColdOperationBehindThunk(jit); }
static void generateColdOperationValue(CCallHelpers& jit) { generateColdOperationBehindThunk(jit, true); }
static void generateColdPlainOperation(CCallHelpers& jit) { generateColdOperationBehindThunk(jit, true, false); }
static void generateColdOperationVoidWithoutFPRs(CCallHelpers& jit) { generateColdOperationBehindThunk(jit, false, true, true, false, false); }
static void generateColdOperationValueWithoutFPRs(CCallHelpers& jit) { generateColdOperationBehindThunk(jit, true, true, true, false, false); }
static void generateColdPlainOperationWithoutFPRs(CCallHelpers& jit) { generateColdOperationBehindThunk(jit, true, false, true, false, false); }
static void generateColdPlainOperationWithoutInstance(CCallHelpers& jit) { generateColdOperationBehindThunk(jit, true, false, false); }
static void generateColdPlainOperationDouble(CCallHelpers& jit) { generateColdOperationBehindThunk(jit, false, false, true, true); }
static void generateColdPlainOperationDoubleWithoutInstance(CCallHelpers& jit) { generateColdOperationBehindThunk(jit, false, false, false, true); }
static void generateColdOperationDouble(CCallHelpers& jit) { generateColdOperationBehindThunk(jit, false, true, true, true); }

static void generateOperationValue(CCallHelpers& jit) { generateOperation(jit, Returns::Value, Supplies::Nothing); }
static void generateOperationVoid(CCallHelpers& jit) { generateOperation(jit, Returns::Void, Supplies::Nothing); }
static void generateOperationDouble(CCallHelpers& jit) { generateOperation(jit, Returns::Double, Supplies::Nothing); }
static void generateOperationValueWithGlobalObject(CCallHelpers& jit) { generateOperation(jit, Returns::Value, Supplies::GlobalObject); }
static void generateOperationValueWithInstance(CCallHelpers& jit) { generateOperation(jit, Returns::Value, Supplies::Instance); }

static void generateStringReplaceRegExpStringThatMayFindNothing(CCallHelpers& jit)
{
    constexpr GPRReg result = GPRInfo::returnValueGPR;
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    constexpr GPRReg string = A1;
    constexpr GPRReg regExpObject = A2;
    constexpr GPRReg replacement = A3;
    constexpr GPRReg scratch = T9;
    constexpr GPRReg impl = T10;
    constexpr GPRReg regExp = T11;
    constexpr GPRReg length = T12;
    static_assert(noOverlap(string, regExpObject, replacement, scratch, impl, regExp, length));
    static_assert(noOverlap(scratch, regExp, A0, A1, A2, A3, A4));
    static_assert(sizeof(InlineWatchpointSet) == sizeof(uintptr_t));
    static_assert(sizeof(std::unique_ptr<Yarr::YarrCodeBlock>) == sizeof(void*));
    static_assert(sizeof(Yarr::MatchingContextHolder) == 4 * sizeof(void*));
    constexpr unsigned offsetOfSavedArguments = roundUpToMultipleOf<sizeof(void*)>(sizeof(Yarr::MatchingContextHolder));
    constexpr unsigned sizeOfFrame = roundUpToMultipleOf<stackAlignmentBytes()>(offsetOfSavedArguments + 3 * sizeof(void*));
    InlineWatchpointSet watchedByNobody(IsWatched);
    uintptr_t wordOfWatchedSet;
    memcpy(&wordOfWatchedSet, &watchedByNobody, sizeof(wordOfWatchedSet));
    CCallHelpers::JumpList isLeftToOperation;

    jit.loadPtr(Address(string, JSString::offsetOfValue()), impl);
    isLeftToOperation.append(jit.branchTestPtr(CCallHelpers::NonZero, impl, TrustedImm32(JSString::isRopeInPointer)));
    isLeftToOperation.append(jit.branchTest32(CCallHelpers::Zero, Address(impl, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));

    loadInstance(jit, regExp);
    jit.loadPtr(Address(regExp, Instance::offsetOfGlobalObject()), regExp);
    jit.load64(Address(regExp, OBJECT_OFFSETOF(JSGlobalObject, m_regExpPrimordialPropertiesWatchpointSet)), scratch);
    isLeftToOperation.append(jit.branch64(CCallHelpers::NotEqual, scratch, CCallHelpers::TrustedImm64(wordOfWatchedSet)));
    jit.load64(Address(regExp, OBJECT_OFFSETOF(JSGlobalObject, m_stringSymbolReplaceWatchpointSet)), scratch);
    isLeftToOperation.append(jit.branch64(CCallHelpers::NotEqual, scratch, CCallHelpers::TrustedImm64(wordOfWatchedSet)));
    jit.load32(Address(regExpObject, JSCell::structureIDOffset()), scratch);
    isLeftToOperation.append(jit.branch32(CCallHelpers::NotEqual, scratch, Address(regExp, OBJECT_OFFSETOF(JSGlobalObject, m_regExpStructure))));
    jit.load64(Address(regExpObject, RegExpObject::offsetOfLastIndex()), scratch);
    isLeftToOperation.append(jit.branchIfNotNumber(scratch));

    jit.load64(Address(regExpObject, RegExpObject::offsetOfRegExpAndFlags()), scratch);
    jit.move(scratch, regExp);
    jit.and64(TrustedImm32(static_cast<int32_t>(RegExpObject::regExpMask)), regExp);
    jit.load16(Address(regExp, RegExp::offsetOfFlags()), length);
    isLeftToOperation.append(jit.branchTest32(CCallHelpers::NonZero, length, TrustedImm32(static_cast<int32_t>(Yarr::Flags::Sticky))));
    isLeftToOperation.append(jit.branch8(CCallHelpers::NotEqual, Address(regExp, RegExp::offsetOfState()), TrustedImm32(RegExp::stateWithJITCode())));
    Jump isNotGlobal = jit.branchTest32(CCallHelpers::Zero, length, TrustedImm32(static_cast<int32_t>(Yarr::Flags::Global)));
    isLeftToOperation.append(jit.branchTest64(CCallHelpers::NonZero, scratch, TrustedImm32(static_cast<int32_t>(RegExpObject::lastIndexIsNotWritableFlag))));
    isNotGlobal.link(&jit);
    jit.loadPtr(Address(regExp, RegExp::offsetOfRegExpJITCode()), scratch);
    jit.loadPtr(Address(scratch, Yarr::YarrCodeBlock::offsetOf8BitCode()), scratch);
    isLeftToOperation.append(jit.branchTestPtr(CCallHelpers::Zero, scratch));
    Jump keepsLastIndex = jit.branchTest32(CCallHelpers::Zero, length, TrustedImm32(static_cast<int32_t>(Yarr::Flags::Global)));
    jit.store64(CCallHelpers::TrustedImm64(JSValue::encode(jsNumber(0))), Address(regExpObject, RegExpObject::offsetOfLastIndex()));
    keepsLastIndex.link(&jit);

    jit.load32(Address(impl, StringImpl::lengthMemoryOffset()), length);
    Jump isTooShort = jit.branch32(CCallHelpers::Below, length, Address(regExp, RegExp::offsetOfMinimumSize()));

    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(sizeOfFrame), sp);
    jit.storePtr(string, Address(sp, offsetOfSavedArguments));
    jit.storePtr(regExpObject, Address(sp, offsetOfSavedArguments + sizeof(void*)));
    jit.storePtr(replacement, Address(sp, offsetOfSavedArguments + 2 * sizeof(void*)));
    jit.move(length, A2);
    jit.loadPtr(Address(impl, StringImpl::dataOffset()), A0);
    jit.loadPtr(Address(regExp, RegExp::offsetOfOvector() + FixedVector<int>::offsetOfStorage()), A3);
    jit.addPtr(TrustedImm32(FixedVector<int>::Storage::offsetOfData()), A3);
    loadInstance(jit, A4);
    jit.loadPtr(Address(A4, Instance::offsetOfVM()), A4);
    jit.loadPtr(Address(A4, VM::offsetOfSoftStackLimit()), A4);
    jit.storePtr(A4, Address(sp, Yarr::MatchingContextHolder::offsetOfStackLimit()));
    jit.move(TrustedImm32(0), A1);
    jit.storePtr(A1, Address(sp, Yarr::MatchingContextHolder::offsetOfFreeList()));
    jit.move(sp, A4);
    jit.call(scratch, Yarr8BitPtrTag);
    Jump isNoMiss = jit.branch64(CCallHelpers::NotEqual, GPRInfo::returnValueGPR, CCallHelpers::TrustedImm64(-1));
    jit.loadPtr(Address(sp, offsetOfSavedArguments), result);
    jit.emitFunctionEpilogue();
    jit.ret();

    isTooShort.link(&jit);
    jit.move(string, result);
    jit.ret();

    isNoMiss.link(&jit);
    jit.loadPtr(Address(sp, offsetOfSavedArguments), string);
    jit.loadPtr(Address(sp, offsetOfSavedArguments + sizeof(void*)), regExpObject);
    jit.loadPtr(Address(sp, offsetOfSavedArguments + 2 * sizeof(void*)), replacement);
    jit.emitFunctionEpilogue();

    isLeftToOperation.link(&jit);
    jit.move(TrustedImm32(static_cast<unsigned>(Entry::operationAOTTypedStringReplaceRegExpString) * sizeof(void*)), T9);
    generateOperation(jit, Returns::Value, Supplies::Instance);
}

static void generateRegExpMatchThatAsksBodyFirst(CCallHelpers& jit, Entry general, Entry found, int64_t resultOfMiss)
{
    constexpr GPRReg result = GPRInfo::returnValueGPR;
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    constexpr GPRReg regExpObject = A1;
    constexpr GPRReg string = A2;
    constexpr GPRReg scratch = T9;
    constexpr GPRReg impl = T10;
    constexpr GPRReg regExp = T11;
    constexpr GPRReg length = T12;
    static_assert(noOverlap(string, regExpObject, scratch, impl, regExp, length));
    static_assert(noOverlap(scratch, regExp, A0, A1, A2, A3, A4));
    static_assert(sizeof(std::unique_ptr<Yarr::YarrCodeBlock>) == sizeof(void*));
    static_assert(sizeof(Yarr::MatchingContextHolder) == 4 * sizeof(void*));
    constexpr unsigned offsetOfSavedArguments = roundUpToMultipleOf<sizeof(void*)>(sizeof(Yarr::MatchingContextHolder));
    constexpr unsigned sizeOfFrame = roundUpToMultipleOf<stackAlignmentBytes()>(offsetOfSavedArguments + 2 * sizeof(void*));
    CCallHelpers::JumpList isLeftToOperation;

    jit.loadPtr(Address(string, JSString::offsetOfValue()), impl);
    isLeftToOperation.append(jit.branchTestPtr(CCallHelpers::NonZero, impl, TrustedImm32(JSString::isRopeInPointer)));
    isLeftToOperation.append(jit.branchTest32(CCallHelpers::Zero, Address(impl, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.load64(Address(regExpObject, RegExpObject::offsetOfLastIndex()), scratch);
    isLeftToOperation.append(jit.branchIfNotNumber(scratch));
    jit.load64(Address(regExpObject, RegExpObject::offsetOfRegExpAndFlags()), regExp);
    jit.and64(TrustedImm32(static_cast<int32_t>(RegExpObject::regExpMask)), regExp);
    jit.load16(Address(regExp, RegExp::offsetOfFlags()), length);
    isLeftToOperation.append(jit.branchTest32(CCallHelpers::NonZero, length, TrustedImm32(static_cast<int32_t>(Yarr::Flags::Global) | static_cast<int32_t>(Yarr::Flags::Sticky))));
    isLeftToOperation.append(jit.branch8(CCallHelpers::NotEqual, Address(regExp, RegExp::offsetOfState()), TrustedImm32(RegExp::stateWithJITCode())));
    jit.loadPtr(Address(regExp, RegExp::offsetOfRegExpJITCode()), scratch);
    jit.loadPtr(Address(scratch, Yarr::YarrCodeBlock::offsetOf8BitCode()), scratch);
    isLeftToOperation.append(jit.branchTestPtr(CCallHelpers::Zero, scratch));
    jit.load32(Address(impl, StringImpl::lengthMemoryOffset()), length);
    Jump isTooShort = jit.branch32(CCallHelpers::Below, length, Address(regExp, RegExp::offsetOfMinimumSize()));

    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(sizeOfFrame), sp);
    jit.storePtr(regExpObject, Address(sp, offsetOfSavedArguments));
    jit.storePtr(string, Address(sp, offsetOfSavedArguments + sizeof(void*)));
    jit.move(length, A2);
    jit.loadPtr(Address(impl, StringImpl::dataOffset()), A0);
    jit.loadPtr(Address(regExp, RegExp::offsetOfOvector() + FixedVector<int>::offsetOfStorage()), A3);
    jit.addPtr(TrustedImm32(FixedVector<int>::Storage::offsetOfData()), A3);
    loadInstance(jit, A4);
    jit.loadPtr(Address(A4, Instance::offsetOfVM()), A4);
    jit.loadPtr(Address(A4, VM::offsetOfSoftStackLimit()), A4);
    jit.storePtr(A4, Address(sp, Yarr::MatchingContextHolder::offsetOfStackLimit()));
    jit.move(TrustedImm32(0), A1);
    jit.storePtr(A1, Address(sp, Yarr::MatchingContextHolder::offsetOfFreeList()));
    jit.move(sp, A4);
    jit.call(scratch, Yarr8BitPtrTag);
    Jump isNoMiss = jit.branch64(CCallHelpers::NotEqual, GPRInfo::returnValueGPR, CCallHelpers::TrustedImm64(-1));
    jit.emitFunctionEpilogue();
    isTooShort.link(&jit);
    jit.move(CCallHelpers::TrustedImm64(resultOfMiss), result);
    jit.ret();

    isNoMiss.link(&jit);
    jit.move(GPRInfo::returnValueGPR, scratch);
    jit.loadPtr(Address(sp, offsetOfSavedArguments), regExpObject);
    jit.loadPtr(Address(sp, offsetOfSavedArguments + sizeof(void*)), string);
    jit.emitFunctionEpilogue();
    isLeftToOperation.append(jit.branch64(CCallHelpers::LessThan, scratch, CCallHelpers::TrustedImm64(0)));
    jit.move(TrustedImm32(static_cast<unsigned>(found) * sizeof(void*)), T9);
    Jump isFound = jit.jump();

    isLeftToOperation.link(&jit);
    jit.move(TrustedImm32(static_cast<unsigned>(general) * sizeof(void*)), T9);
    isFound.link(&jit);
    generateOperation(jit, Returns::Value, Supplies::Instance);
}
static void generateRegExpExecThatAsksBodyFirst(CCallHelpers& jit) { generateRegExpMatchThatAsksBodyFirst(jit, Entry::operationAOTTypedRegExpExec, Entry::operationAOTTypedRegExpExecFound, JSValue::encode(jsNull())); }

static void fenceAfterAllocation(CCallHelpers&, GPRReg scratch);

static void generateNewRegExpLiteral(CCallHelpers& jit, bool checksException)
{
    constexpr GPRReg result = GPRInfo::returnValueGPR;
    constexpr GPRReg place = A3;
    constexpr GPRReg shared = T9;
    constexpr GPRReg allocator = T10;
    constexpr GPRReg scratch = T11;
    constexpr auto kind = Instance::InlineAllocation::RegExpObject;
    static_assert(noOverlap(result, A1, A2, place, shared, allocator, scratch));
    static_assert(sizeof(RegExpObject) == 4 * sizeof(uint64_t) && JSObject::butterflyOffset() == sizeof(uint64_t) && RegExpObject::offsetOfRegExpAndFlags() == 2 * sizeof(uint64_t) && RegExpObject::offsetOfLastIndex() == 3 * sizeof(uint64_t));
    CCallHelpers::JumpList isLeftToOperation;
    isLeftToOperation.append(jit.branch32(CCallHelpers::Equal, place, TrustedImm32(-1)));
    jit.zeroExtend32ToWord(place, scratch);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfTypedStatics()), shared);
    jit.loadPtr(CCallHelpers::BaseIndex(shared, scratch, CCallHelpers::TimesOne), shared);
    isLeftToOperation.append(jit.branchTestPtr(CCallHelpers::Zero, shared));
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfAllocatorFor(kind)), allocator);
    isLeftToOperation.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    jit.emitAllocateWithNonNullAllocator(result, JITAllocator::variable(), allocator, scratch, isLeftToOperation, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
    jit.load64(Address(instanceGPR, Instance::offsetOfHeaderFor(kind)), scratch);
    jit.store64(scratch, Address(result));
    jit.store64(TrustedImm32(0), Address(result, JSObject::butterflyOffset()));
    jit.load64(Address(shared, RegExpObject::offsetOfRegExpAndFlags()), scratch);
    jit.and64(TrustedImm32(static_cast<int32_t>(RegExpObject::regExpMask)), scratch);
    jit.store64(scratch, Address(result, RegExpObject::offsetOfRegExpAndFlags()));
    jit.move(CCallHelpers::TrustedImm64(JSValue::encode(jsNumber(0))), scratch);
    jit.store64(scratch, Address(result, RegExpObject::offsetOfLastIndex()));
    fenceAfterAllocation(jit, scratch);
    if (!checksException)
        jit.move(TrustedImm32(0), GPRInfo::returnValueGPR2);
    jit.ret();

    isLeftToOperation.link(&jit);
    jit.move(TrustedImm32(static_cast<unsigned>(Entry::operationAOTTypedNewRegExpLiteral) * sizeof(void*)), T9);
    generateOperation(jit, Returns::Value, Supplies::Instance, GPRInfo::returnValueGPR, checksException);
}
static void generateNewRegExpLiteral(CCallHelpers& jit) { generateNewRegExpLiteral(jit, true); }
static void generateUncheckedNewRegExpLiteral(CCallHelpers& jit) { generateNewRegExpLiteral(jit, false); }
static void generateRegExpTestThatAsksBodyFirst(CCallHelpers& jit) { generateRegExpMatchThatAsksBodyFirst(jit, Entry::operationAOTTypedRegExpTest, Entry::operationAOTTypedRegExpTestFound, 0); }

static void loadBaseAndOffsetOfSubstring(CCallHelpers&, GPRReg string, GPRReg base, GPRReg offset);

static void generateStringInCase(CCallHelpers& jit, bool isUpper, bool checksException)
{
    constexpr GPRReg result = GPRInfo::returnValueGPR;
    constexpr GPRReg string = A1;
    constexpr GPRReg characters = T12;
    constexpr GPRReg end = A2;
    constexpr GPRReg word = A3;
    constexpr GPRReg fromFirstLetter = T9;
    constexpr GPRReg toLastLetter = T10;
    constexpr GPRReg first = T11;
    constexpr GPRReg second = result;
    static_assert(noOverlap(string, characters, end, word, fromFirstLetter, toLastLetter, first, second));
    using BaseIndex = CCallHelpers::BaseIndex;
    CCallHelpers::JumpList isLeftToOperation;
    CCallHelpers::JumpList lastIsLoaded;
    auto leaveIfSomethingChanges = [&] {
        jit.add64(word, fromFirstLetter, first);
        jit.sub64(toLastLetter, word, second);
        jit.and64(second, first);
        jit.or64(word, first);
        isLeftToOperation.append(jit.branchTest64(CCallHelpers::NonZero, first, CCallHelpers::TrustedImm64(static_cast<int64_t>(0x8080808080808080ULL))));
    };
    jit.loadPtr(Address(string, JSString::offsetOfValue()), characters);
    Jump isRope = jit.branchIfRopeStringImpl(characters);
    jit.load32(Address(characters, StringImpl::lengthMemoryOffset()), end);
    isLeftToOperation.append(jit.branchTest32(CCallHelpers::Zero, Address(characters, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.loadPtr(Address(characters, StringImpl::dataOffset()), characters);
    auto hasCharacters = jit.label();
    jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(isUpper ? 0x1f1f1f1f1f1f1f1fULL : 0x3f3f3f3f3f3f3f3fULL)), fromFirstLetter);
    jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(isUpper ? 0xfafafafafafafafaULL : 0xdadadadadadadadaULL)), toLastLetter);
    Jump isBelow8 = jit.branch32(CCallHelpers::Below, end, TrustedImm32(8));
    jit.addPtr(characters, end);
    jit.subPtr(TrustedImm32(8), end);
    auto nextWord = jit.label();
    jit.load64(Address(characters), word);
    leaveIfSomethingChanges();
    jit.addPtr(TrustedImm32(8), characters);
    jit.branchPtr(CCallHelpers::Below, characters, end).linkTo(nextWord, &jit);
    jit.load64(Address(end), word);
    lastIsLoaded.append(jit.jump());

    isBelow8.link(&jit);
    Jump isBelow4 = jit.branch32(CCallHelpers::Below, end, TrustedImm32(4));
    jit.load32(Address(characters), word);
    jit.sub32(TrustedImm32(4), end);
    jit.load32(BaseIndex(characters, end, CCallHelpers::TimesOne), first);
    jit.lshift64(TrustedImm32(32), first);
    jit.or64(first, word);
    lastIsLoaded.append(jit.jump());

    isBelow4.link(&jit);
    Jump isBelow2 = jit.branch32(CCallHelpers::Below, end, TrustedImm32(2));
    jit.load16(Address(characters), word);
    jit.sub32(TrustedImm32(2), end);
    jit.load16(BaseIndex(characters, end, CCallHelpers::TimesOne), first);
    jit.lshift64(TrustedImm32(16), first);
    jit.or64(first, word);
    lastIsLoaded.append(jit.jump());

    isBelow2.link(&jit);
    Jump isEmpty = jit.branchTest32(CCallHelpers::Zero, end);
    jit.load8(Address(characters), word);

    lastIsLoaded.link(&jit);
    leaveIfSomethingChanges();
    isEmpty.link(&jit);
    jit.move(string, result);
    if (!checksException)
        jit.move(TrustedImm32(0), GPRInfo::returnValueGPR2);
    jit.ret();

    isRope.link(&jit);
    isLeftToOperation.append(jit.branchTestPtr(CCallHelpers::Zero, characters, TrustedImm32(JSRopeString::isSubstringInPointer)));
    loadBaseAndOffsetOfSubstring(jit, string, characters, first);
    jit.loadPtr(Address(characters, JSString::offsetOfValue()), characters);
    isLeftToOperation.append(jit.branchTest32(CCallHelpers::Zero, Address(characters, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.loadPtr(Address(characters, StringImpl::dataOffset()), characters);
    jit.addPtr(first, characters);
    jit.load32(Address(string, JSRopeString::offsetOfLength()), end);
    jit.jump().linkTo(hasCharacters, &jit);

    isLeftToOperation.link(&jit);
    jit.move(TrustedImm32(static_cast<unsigned>(isUpper ? Entry::operationAOTTypedStringToUpperCase : Entry::operationAOTTypedStringToLowerCase) * sizeof(void*)), T9);
    generateOperation(jit, Returns::Value, Supplies::Instance, GPRInfo::returnValueGPR, checksException);
}
static void generateStringToLowerCase(CCallHelpers& jit) { generateStringInCase(jit, false, true); }
static void generateStringToUpperCase(CCallHelpers& jit) { generateStringInCase(jit, true, true); }
static void generateUncheckedStringToLowerCase(CCallHelpers& jit) { generateStringInCase(jit, false, false); }
static void generateUncheckedStringToUpperCase(CCallHelpers& jit) { generateStringInCase(jit, true, false); }

static void fenceAfterAllocation(CCallHelpers&, GPRReg scratch);

static void generateNewTypedArray(CCallHelpers& jit, bool checksException)
{
    using Row = Instance::TypedArrayAllocation;
    constexpr GPRReg result = GPRInfo::returnValueGPR;
    constexpr GPRReg type = A1;
    constexpr GPRReg length = A2;
    constexpr GPRReg size = A3;
    constexpr GPRReg storage = T9;
    constexpr GPRReg allocator = T10;
    constexpr GPRReg scratch = T11;
    constexpr GPRReg row = T12;
    constexpr unsigned stepShift = getLSBSet(MarkedSpace::sizeStep);
    constexpr unsigned maxBytes = 256;
    static_assert(noOverlap(result, type, length, size, storage, allocator, scratch, row));
    static_assert(hasOneBitSet(MarkedSpace::sizeStep) && sizeof(Row) == 32 && JSArrayBufferView::offsetOfByteOffset() == JSArrayBufferView::offsetOfLength() + static_cast<ptrdiff_t>(sizeof(uint64_t)));
    static_assert(maxBytes <= JSArrayBufferView::fastSizeLimit && maxBytes <= MarkedSpace::largeCutoff);
    auto inRow = [&](ptrdiff_t offset) { return Address(row, Instance::offsetOfTypedArrayAllocations() + offset); };
    CCallHelpers::JumpList isLeftToOperation;
    jit.branchConvertDoubleToInt32(FPRInfo::argumentFPR0, length, isLeftToOperation, FPRInfo::argumentFPR1, false);
    jit.sub32(length, TrustedImm32(1), scratch);
    isLeftToOperation.append(jit.branch32(CCallHelpers::AboveOrEqual, scratch, TrustedImm32(maxBytes)));
    jit.zeroExtend32ToWord(type, row);
    jit.lshift64(TrustedImm32(5), row);
    jit.addPtr(instanceGPR, row);
    isLeftToOperation.append(jit.branchTestPtr(CCallHelpers::Zero, inRow(OBJECT_OFFSETOF(Row, allocator))));
    jit.load32(inRow(OBJECT_OFFSETOF(Row, elementSize)), size);
    jit.mul32(length, size);
    jit.add32(TrustedImm32(7), size);
    jit.and32(TrustedImm32(~7), size);
    isLeftToOperation.append(jit.branch32(CCallHelpers::Above, size, TrustedImm32(maxBytes)));
    jit.add32(TrustedImm32(MarkedSpace::sizeStep - 1), size, scratch);
    jit.urshift32(TrustedImm32(stepShift), scratch);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfPrimitiveAuxiliarySpace()), allocator);
    jit.loadPtr(CCallHelpers::BaseIndex(allocator, scratch, CCallHelpers::ScalePtr, CompleteSubspace::offsetOfAllocatorForSizeStep()), allocator);
    isLeftToOperation.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    jit.emitAllocateWithNonNullAllocator(storage, JITAllocator::variable(), allocator, scratch, isLeftToOperation, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
    jit.urshift32(TrustedImm32(3), size);
    auto nextWord = jit.label();
    jit.sub32(TrustedImm32(1), size);
    jit.store64(TrustedImm32(0), CCallHelpers::BaseIndex(storage, size, CCallHelpers::TimesEight));
    jit.branchTest32(CCallHelpers::NonZero, size).linkTo(nextWord, &jit);
    jit.loadPtr(inRow(OBJECT_OFFSETOF(Row, allocator)), allocator);
    jit.emitAllocateWithNonNullAllocator(result, JITAllocator::variable(), allocator, scratch, isLeftToOperation, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
    jit.load64(inRow(OBJECT_OFFSETOF(Row, header)), scratch);
    jit.store64(scratch, Address(result));
    jit.store64(TrustedImm32(0), Address(result, JSObject::butterflyOffset()));
    jit.storePtr(storage, Address(result, JSArrayBufferView::offsetOfVector()));
    jit.store64(length, Address(result, JSArrayBufferView::offsetOfLength()));
    jit.store64(TrustedImm32(0), Address(result, JSArrayBufferView::offsetOfByteOffset()));
    jit.store8(TrustedImm32(FastTypedArray), Address(result, JSArrayBufferView::offsetOfMode()));
    fenceAfterAllocation(jit, scratch);
    if (!checksException)
        jit.move(TrustedImm32(0), GPRInfo::returnValueGPR2);
    jit.ret();

    isLeftToOperation.link(&jit);
    jit.move(TrustedImm32(static_cast<unsigned>(Entry::operationAOTTypedNewTypedArray) * sizeof(void*)), T9);
    generateOperation(jit, Returns::Value, Supplies::Instance, GPRInfo::returnValueGPR, checksException);
}
static void generateNewTypedArray(CCallHelpers& jit) { generateNewTypedArray(jit, true); }
static void generateUncheckedNewTypedArray(CCallHelpers& jit) { generateNewTypedArray(jit, false); }
static void generateOperationVoidWithInstance(CCallHelpers& jit) { generateOperation(jit, Returns::Void, Supplies::Instance); }
static void generateOperationDoubleWithInstance(CCallHelpers& jit) { generateOperation(jit, Returns::Double, Supplies::Instance); }
static void generateUncheckedOperationWithInstance(CCallHelpers& jit) { generateOperation(jit, Returns::Value, Supplies::Instance, GPRInfo::returnValueGPR, false); }
static void generateUncheckedOperationDoubleWithInstance(CCallHelpers& jit) { generateOperation(jit, Returns::Double, Supplies::Instance, GPRInfo::returnValueGPR, false); }

static void generatePlain(CCallHelpers& jit, std::optional<ptrdiff_t> firstArgument, bool suppliesInstance = false)
{
    loadInstance(jit, operationGPR);
    if (firstArgument)
        jit.loadPtr(Address(operationGPR, *firstArgument), A0);
    if (suppliesInstance)
        jit.move(operationGPR, A0);
    jit.loadPtr(Address(operationGPR, Instance::offsetOfRuntimeTable()), operationGPR);
    jit.loadPtr(CCallHelpers::BaseIndex(operationGPR, T9, CCallHelpers::TimesOne), operationGPR);
    jit.farJump(operationGPR, OperationPtrTag);
}

static void generatePlainOperation(CCallHelpers& jit) { generatePlain(jit, std::nullopt); }
static void generatePlainOperationWithInstance(CCallHelpers& jit) { generatePlain(jit, std::nullopt, true); }

// A1: an object of the program. A2: the offset of a slot in the cell, whose field has been found absent and is about to be stored to. The
// field is the last of the keys from here on. See Instance::typedOrderKeys. A3: the slot of the site, or 0. It is left with the order word and the shape word as they were in `pointer`, and
// the order word as it is now in `offset`: see Lowering::didCreateFieldInCell().
// `isLeaf`: the caller has no frame and has handed over its own return address in T10, which is worked with here: it waits on the stack.
static void generateDidCreateTypedField(CCallHelpers& jit, bool isLeaf)
{
    auto leave = [&] {
#if CPU(ARM64)
        if (isLeaf) {
            jit.move(CCallHelpers::linkRegister, ARM64Registers::x16);
            jit.popToRestore(CCallHelpers::linkRegister);
            jit.m_assembler.ret(ARM64Registers::x16);
            return;
        }
#endif
        jit.ret();
    };
    if (isLeaf)
        jit.pushToSave(T10);
    CCallHelpers::JumpList slow;
    CCallHelpers::JumpList isNotNextInOrder;
    CCallHelpers::JumpList done;
    static_assert(noOverlap(A1, A2, A3, T9, T10, T11, T12, T13));
    jit.load32(Address(A1, Typed::offsetOfOrderWord), T9);
    jit.load32(Address(A1, Typed::offsetOfShapeWord), T10);
    static_assert(Typed::shapeOf(Typed::shapeWordOf(5)) == 5 && Typed::shapeWordOf(5) == 0u - 5);
    jit.neg32(T10);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfTypedOrderKeys()), T11);
    jit.loadPtr(CCallHelpers::BaseIndex(T11, T10, CCallHelpers::TimesEight), T11);
    slow.append(jit.branchTestPtr(CCallHelpers::Zero, T11));
    jit.and32(TrustedImm32(0xffff), T9, T10);
    slow.append(jit.branch32(CCallHelpers::AboveOrEqual, T10, Address(T11)));
    jit.loadPtr(CCallHelpers::BaseIndex(T11, T10, CCallHelpers::TimesEight, sizeof(uintptr_t)), T11);
    // With the count: whether it has extra properties, is frozen, sealed, not extensible. No order is that long.
    static_assert(Typed::maxFieldsInOrder < 1u << 12 && Typed::orderWordHasExtraProperties == 1u << 28);
    jit.urshift32(T9, TrustedImm32(16), T10);
    jit.load16(Address(T11), T13);
    isNotNextInOrder.append(jit.branch32(CCallHelpers::AboveOrEqual, T10, T13));
    jit.load16(CCallHelpers::BaseIndex(T11, T10, CCallHelpers::TimesTwo, sizeof(uint16_t)), T12);
    isNotNextInOrder.append(jit.branch32(CCallHelpers::NotEqual, T12, A2));
    jit.add32(TrustedImm32(1 << 16), T9, T10);
    jit.store32(T10, Address(A1, Typed::offsetOfOrderWord));
    done.append(jit.jump());

    // The pair at the count, behind the keys: the slot that was created there the last time, and the order from there on.
    isNotNextInOrder.link(&jit);
    slow.append(jit.branch32(CCallHelpers::Above, T10, T13));
    jit.getEffectiveAddress(CCallHelpers::BaseIndex(T11, T13, CCallHelpers::TimesTwo), T11);
    jit.getEffectiveAddress(CCallHelpers::BaseIndex(T11, T10, CCallHelpers::TimesFour), T11);
    jit.load16(Address(T11, sizeof(uint16_t)), T12);
    slow.append(jit.branch32(CCallHelpers::NotEqual, T12, A2));
    jit.load16(Address(T11, 2 * sizeof(uint16_t)), T12);
    jit.add32(TrustedImm32(1), T10);
    jit.lshift32(TrustedImm32(16), T10);
    jit.or32(T12, T10);
    jit.store32(T10, Address(A1, Typed::offsetOfOrderWord));

    // T9: the order word as it was. T10: as it is.
    done.link(&jit);
    Jump hasNoSlot = jit.branchTestPtr(CCallHelpers::Zero, A3);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfSharedData()), T11);
    jit.subPtr(A3, T11, T11);
    jit.addPtr(TrustedImm32(BiasedDataPointer::bias), T11);
    jit.move(CCallHelpers::TrustedImmPtr(SharedData::size), T12);
    Jump isNobodys = jit.branchPtr(CCallHelpers::Below, T11, T12);
    jit.xor32(T9, T10, T11);
    jit.store32(T11, Address(A3, OBJECT_OFFSETOF(Slot, offset)));
    jit.store32(T9, Address(A3, OBJECT_OFFSETOF(Slot, pointer)));
    jit.load32(Address(A1, Typed::offsetOfShapeWord), T11);
    jit.store32(T11, Address(A3, OBJECT_OFFSETOF(Slot, pointer) + sizeof(uint32_t)));
    hasNoSlot.link(&jit);
    leave();

    isNobodys.link(&jit);
    loadCallerIndex(jit);
    loadDataForSlot(jit, A3, T10);
    countSlotMiss(jit, T10, true);
    leave();

    slow.link(&jit);
    jit.move(TrustedImm32(static_cast<unsigned>(Entry::operationAOTTypedDidCreateFieldAt) * sizeof(void*)), T9);
#if CPU(ARM64)
    // The site counts on every register but the temporaries: see Lowering::didCreateFieldInCell().
    if (isLeaf)
        jit.popToRestore(T10);
    generateColdOperation(jit, isLeaf, false, false);
#else
    generatePlain(jit, std::nullopt, true);
#endif
}
static void generateDidCreateTypedField(CCallHelpers& jit) { generateDidCreateTypedField(jit, false); }
#if CPU(ARM64)
static void generateLeafDidCreateTypedField(CCallHelpers& jit) { generateDidCreateTypedField(jit, true); }
#else
static void generateLeafDidCreateTypedField(CCallHelpers& jit) { jit.breakpoint(); }
#endif

template<typename Functor>
static void preservingRegistersOfCaller(CCallHelpers& jit, bool hasResult, const Functor& functor)
{
    RegisterSet toSave = RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters());
    Vector<Reg, 48> registers;
    toSave.forEach([&](Reg reg) {
        if (!hasResult || reg != Reg(GPRInfo::returnValueGPR))
            registers.append(reg);
    });
#if CPU(X86_64)
    unsigned bytes = WTF::roundUpToMultipleOf<stackAlignmentBytes()>((registers.size() + 2) * sizeof(CPURegister)) - sizeof(CPURegister);
    jit.subPtr(TrustedImm32(bytes), CCallHelpers::stackPointerRegister);
#else
    unsigned bytes = WTF::roundUpToMultipleOf<stackAlignmentBytes()>((registers.size() + 1) * sizeof(CPURegister));
    jit.subPtr(TrustedImm32(bytes), CCallHelpers::stackPointerRegister);
    jit.storePtr(CCallHelpers::linkRegister, Address(CCallHelpers::stackPointerRegister));
#endif
    for (unsigned i = 0; i < registers.size(); ++i) {
        Address address(CCallHelpers::stackPointerRegister, (i + 1) * sizeof(CPURegister));
#if CPU(ARM64)
        if (i + 1 < registers.size() && registers[i].isGPR() == registers[i + 1].isGPR()) {
            if (registers[i].isGPR())
                jit.storePair64(registers[i].gpr(), registers[i + 1].gpr(), address);
            else
                jit.storePairDouble(registers[i].fpr(), registers[i + 1].fpr(), address);
            ++i;
            continue;
        }
#endif
        if (registers[i].isGPR())
            jit.storePtr(registers[i].gpr(), address);
        else
            jit.storeDouble(registers[i].fpr(), address);
    }
    functor(bytes);
    for (unsigned i = 0; i < registers.size(); ++i) {
        Address address(CCallHelpers::stackPointerRegister, (i + 1) * sizeof(CPURegister));
#if CPU(ARM64)
        if (i + 1 < registers.size() && registers[i].isGPR() == registers[i + 1].isGPR()) {
            if (registers[i].isGPR())
                jit.loadPair64(address, registers[i].gpr(), registers[i + 1].gpr());
            else
                jit.loadPairDouble(address, registers[i].fpr(), registers[i + 1].fpr());
            ++i;
            continue;
        }
#endif
        if (registers[i].isGPR())
            jit.loadPtr(address, registers[i].gpr());
        else
            jit.loadDouble(address, registers[i].fpr());
    }
#if !CPU(X86_64)
    jit.loadPtr(Address(CCallHelpers::stackPointerRegister), CCallHelpers::linkRegister);
#endif
    jit.addPtr(TrustedImm32(bytes), CCallHelpers::stackPointerRegister);
}

static void loadReturnAddressOfCaller(CCallHelpers& jit, unsigned bytesPushed, GPRReg result)
{
#if CPU(X86_64)
    jit.loadPtr(Address(CCallHelpers::stackPointerRegister, bytesPushed), result);
#else
    UNUSED_PARAM(bytesPushed);
    jit.move(CCallHelpers::linkRegister, result);
#endif
}

// To code compiled from types, which has handed over its own return address in T10 if it has no frame.
static void returnToTypedCaller(CCallHelpers& jit, bool isLeaf)
{
#if CPU(ARM64)
    if (isLeaf) {
        jit.move(CCallHelpers::linkRegister, ARM64Registers::x16);
        jit.move(T10, CCallHelpers::linkRegister);
        jit.m_assembler.ret(ARM64Registers::x16);
        return;
    }
#else
    RELEASE_ASSERT(!isLeaf);
#endif
    jit.ret();
}

#if CPU(ARM64)
static bool generateBarrierKeepingRegisters(CCallHelpers& jit, Stub stub)
{
    bool isLeaf = stub == Stub::LeafColdPlainOperation || stub == Stub::LeafColdPlainOperationWithoutFPRs;
    if (!isLeaf && stub != Stub::ColdPlainOperation && stub != Stub::ColdPlainOperationWithoutFPRs)
        return false;
    constexpr GPRReg link = CCallHelpers::linkRegister;
    constexpr GPRReg formerArgument = ARM64Registers::x16;
    constexpr GPRReg linkOfCallerWithoutFrame = ARM64Registers::x17;
    GPRReg temporary = isLeaf ? T9 : ARM64Registers::x17;
    static_assert(Instance::offsetOfMutatorShouldBeFenced() < 4096);
    if (isLeaf)
        jit.pushPair(T9, T10);
    jit.load8(Address(instanceGPR, Instance::offsetOfMutatorShouldBeFenced()), temporary);
    Jump isNotFenced = jit.branchTest32(CCallHelpers::Zero, temporary);
    jit.memoryFence();
    jit.load8(Address(A1, JSCell::cellStateOffset()), temporary);
    Jump isBlack = jit.branch32(CCallHelpers::BelowOrEqual, temporary, TrustedImm32(blackThreshold));
    if (isLeaf)
        jit.popPair(T9, T10);
    jit.move(formerArgument, A1);
    if (isLeaf) {
        jit.move(link, formerArgument);
        jit.move(linkOfCallerWithoutFrame, link);
        jit.m_assembler.ret(formerArgument);
    } else
        jit.ret();
    isNotFenced.link(&jit);
    isBlack.link(&jit);
    jit.pushPair(GPRInfo::returnValueGPR, formerArgument);
    jit.pushPair(isLeaf ? linkOfCallerWithoutFrame : T9, link);
    if (isLeaf)
        jit.move(linkOfCallerWithoutFrame, T10);
    jit.move(TrustedImm32(static_cast<unsigned>(Entry::operationAOTTypedBarrierSlowKeepingRegisters) * sizeof(void*)), T9);
    s_callsBetweenStubs->append({ jit.nearCall(), stub });
    jit.popPair(isLeaf ? linkOfCallerWithoutFrame : T9, isLeaf ? formerArgument : link);
    if (isLeaf) {
        jit.move(formerArgument, linkOfCallerWithoutFrame);
        jit.popPair(GPRInfo::returnValueGPR, A1);
        jit.popPair(T9, T10);
        jit.m_assembler.ret(linkOfCallerWithoutFrame);
        return true;
    }
    jit.popPair(GPRInfo::returnValueGPR, A1);
    jit.ret();
    return true;
}
#endif

static void returnFromBarrierUnlessOwnerIsBlack(CCallHelpers& jit, Stub stub)
{
    bool isLeaf = stub == Stub::LeafColdPlainOperation || stub == Stub::LeafColdPlainOperationWithoutFPRs;
    if ((!isLeaf && stub != Stub::ColdPlainOperation && stub != Stub::ColdPlainOperationWithoutFPRs) || (isLeaf && !isARM64()))
        return;
    GPRReg temporary = numberOfColdOperationGPR();
    jit.load8(Address(instanceGPR, Instance::offsetOfMutatorShouldBeFenced()), temporary);
    Jump isNotFenced = jit.branchTest32(CCallHelpers::Zero, temporary);
    jit.memoryFence();
    jit.load8(Address(operationArgumentGPR(1), JSCell::cellStateOffset()), temporary);
    Jump isBlack = jit.branch32(CCallHelpers::BelowOrEqual, temporary, TrustedImm32(blackThreshold));
    returnToTypedCaller(jit, isLeaf);
    isNotFenced.link(&jit);
    isBlack.link(&jit);
}

// T9: an array that is within the threshold of the barrier. T11: what has been stored into it, boxed. See Typed::State::rememberElement(). On ARM64 every register is kept but the temporaries.
static void generateRememberElement(CCallHelpers& jit, bool isLeaf)
{
    static_assert(!static_cast<unsigned>(CellState::PossiblyBlack));
    CCallHelpers::JumpList slow;
    CCallHelpers::JumpList done;
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfVM()), T12);
    Jump isFenced = jit.branchTest8(CCallHelpers::NonZero, Address(T12, VM::offsetOfHeapMutatorShouldBeFenced()));
    jit.loadPtr(Address(T9, JSObject::butterflyOffset()), T12);
    slow.append(jit.branch32(CCallHelpers::Below, Address(T12, Butterfly::offsetOfVectorLength()), TrustedImm32(Typed::State::minVectorLengthToRememberElements)));
    done.append(jit.branchIfNotCell(T11));
    done.append(jit.branchTest8(CCallHelpers::Zero, Address(T11, JSCell::cellStateOffset())));
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfYoungRootsCursor()), T12);
    slow.append(jit.branchPtr(CCallHelpers::Equal, T12, Address(instanceGPR, Instance::offsetOfYoungRootsEnd())));
    jit.storePtr(T11, Address(T12));
    jit.addPtr(TrustedImm32(sizeof(JSCell*)), T12);
    jit.storePtr(T12, Address(instanceGPR, Instance::offsetOfYoungRootsCursor()));
    done.link(&jit);
    returnToTypedCaller(jit, isLeaf);

    isFenced.link(&jit);
    jit.memoryFence();
    slow.append(jit.barrierBranchWithoutFence(T9, true));
    returnToTypedCaller(jit, isLeaf);

    slow.link(&jit);
#if CPU(ARM64)
    jit.move(T9, T12);
    jit.move(T11, T13);
    jit.move(TrustedImm32(static_cast<unsigned>(Entry::operationAOTTypedRememberElement) * sizeof(void*)), T9);
    generateColdOperation(jit, isLeaf, false, false, true, false, true, std::nullopt, T12, T13);
#else
    preservingRegistersOfCaller(jit, false, [&](unsigned) {
        jit.move(T9, A1);
        jit.move(T11, A2);
        jit.move(instanceGPR, A0);
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T11);
        jit.loadPtr(Address(T11, static_cast<unsigned>(Entry::operationAOTTypedRememberElement) * sizeof(void*)), T11);
        jit.call(T11, OperationPtrTag);
    });
    jit.ret();
#endif
}
static void generateRememberElement(CCallHelpers& jit) { generateRememberElement(jit, false); }
#if CPU(ARM64)
static void generateLeafRememberElement(CCallHelpers& jit) { generateRememberElement(jit, true); }
#else
static void generateLeafRememberElement(CCallHelpers& jit) { jit.breakpoint(); }
#endif

enum class TypedCellInit : uint8_t { Header, HeaderAndWords, FromTemplate };

// T9: the number of the allocation, see Instance::typedAllocationRows. Yields the cell. On ARM64 every register is kept but that and the temporaries.
static void generateAllocateTyped(CCallHelpers& jit, bool isLeaf, TypedCellInit init)
{
    constexpr GPRReg cell = GPRInfo::returnValueGPR;
    static_assert(sizeof(Instance::TypedAllocation) == 32);
    CCallHelpers::JumpList slow;
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfTypedAllocationRows()), T11);
#if CPU(ARM64)
    jit.load16(CCallHelpers::BaseIndex(T11, T9, CCallHelpers::TimesTwo, 0, CCallHelpers::Extend::ZExt32), T12);
#else
    jit.zeroExtend32ToWord(T9, T12);
    jit.load16(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesTwo), T12);
#endif
    // The first allocation, which takes the slow path, gets the row. A row has an allocator.
    slow.append(jit.branchTest32(CCallHelpers::Zero, T12));
#if CPU(ARM64)
    jit.subLeftShift64(T11, T12, TrustedImm32(5), T11);
#else
    jit.lshift64(TrustedImm32(5), T12);
    jit.subPtr(T12, T11);
#endif
    jit.loadPtr(Address(T11, OBJECT_OFFSETOF(Instance::TypedAllocation, allocator)), T12);
    constexpr int32_t intervalStart = LocalAllocator::offsetOfFreeList() + FreeList::offsetOfIntervalStart();
    constexpr int32_t intervalEnd = LocalAllocator::offsetOfFreeList() + FreeList::offsetOfIntervalEnd();
    constexpr int32_t nextInterval = LocalAllocator::offsetOfFreeList() + FreeList::offsetOfNextInterval();
    if (Options::forceGCSlowPaths())
        slow.append(jit.jump());
#if CPU(ARM64)
    static_assert(intervalEnd - intervalStart == sizeof(uintptr_t));
    static_assert(nextInterval - intervalEnd == sizeof(uintptr_t));
    static_assert(FreeList::offsetOfSecret() - FreeList::offsetOfNextInterval() == sizeof(uintptr_t));
    jit.loadPairPtr(T12, TrustedImm32(intervalStart), cell, T13);
    Jump intervalIsUsedUp = jit.branchPtr(CCallHelpers::AboveOrEqual, cell, T13);
    auto bump = jit.label();
    jit.load32(Address(T12, LocalAllocator::offsetOfCellSize()), T13);
    jit.addPtr(cell, T13);
    jit.storePtr(T13, Address(T12, intervalStart));
#else
    jit.loadPtr(Address(T12, intervalStart), cell);
    Jump intervalIsUsedUp = jit.branchPtr(CCallHelpers::AboveOrEqual, cell, Address(T12, intervalEnd));
    auto bump = jit.label();
    jit.load32(Address(T12, LocalAllocator::offsetOfCellSize()), T13);
    jit.add64(T13, Address(T12, intervalStart));
#endif
    if (init == TypedCellInit::FromTemplate) {
        constexpr GPRReg end = T13;
        constexpr GPRReg from = T12;
        constexpr GPRReg to = T14;
        jit.load32(Address(T12, LocalAllocator::offsetOfCellSize()), end);
        jit.add64(cell, end);
        jit.loadPtr(Address(T11, OBJECT_OFFSETOF(Instance::TypedAllocation, fieldTemplate)), from);
        jit.add64(TrustedImm32(sizeof(JSCell) + sizeof(uint64_t)), cell, to);
        Jump done = jit.branch64(CCallHelpers::AboveOrEqual, to, end);
        auto loop = jit.label();
#if CPU(ARM64)
        jit.loadPair64(CCallHelpers::PostIndexAddress(from, 16), T9, T15);
        jit.storePair64(T9, T15, CCallHelpers::PostIndexAddress(to, 16));
#else
        // T15 is T9 here.
        jit.load64(Address(from), T9);
        jit.store64(T9, Address(to));
        jit.load64(Address(from, 8), T9);
        jit.store64(T9, Address(to, 8));
        jit.add64(TrustedImm32(16), from);
        jit.add64(TrustedImm32(16), to);
#endif
        jit.branch64(CCallHelpers::Below, to, end).linkTo(loop, &jit);
        done.link(&jit);
    }
#if CPU(ARM64)
    static_assert(OBJECT_OFFSETOF(Instance::TypedAllocation, words) == OBJECT_OFFSETOF(Instance::TypedAllocation, header) + sizeof(uint64_t) && sizeof(JSCell) == sizeof(uint64_t));
    if (init != TypedCellInit::Header) {
        jit.loadPair64(T11, TrustedImm32(OBJECT_OFFSETOF(Instance::TypedAllocation, header)), T12, T13);
        jit.storePair64(T12, T13, cell, TrustedImm32(0));
    } else {
        jit.load64(Address(T11, OBJECT_OFFSETOF(Instance::TypedAllocation, header)), T13);
        jit.store64(T13, Address(cell, 0));
    }
#else
    jit.load64(Address(T11, OBJECT_OFFSETOF(Instance::TypedAllocation, header)), T13);
    jit.store64(T13, Address(cell, 0));
    if (init != TypedCellInit::Header) {
        jit.load64(Address(T11, OBJECT_OFFSETOF(Instance::TypedAllocation, words)), T13);
        jit.store64(T13, Address(cell, sizeof(JSCell)));
    }
#endif
    returnToTypedCaller(jit, isLeaf);

    intervalIsUsedUp.link(&jit);
#if CPU(ARM64)
    GPRReg temporary = jit.scratchRegister();
    jit.loadPairPtr(T12, TrustedImm32(nextInterval), cell, T13);
    slow.append(jit.branchTestPtr(CCallHelpers::NonZero, cell, TrustedImm32(1)));
    jit.xor64(Address(cell, FreeCell::offsetOfScrambledBits()), T13);
    jit.addSignExtend64(cell, T13, temporary);
    jit.addUnsignedRightShift64(cell, T13, TrustedImm32(32), T13);
    jit.storePairPtr(T13, temporary, T12, TrustedImm32(intervalEnd));
#else
    constexpr int32_t secret = LocalAllocator::offsetOfFreeList() + FreeList::offsetOfSecret();
    jit.loadPtr(Address(T12, nextInterval), cell);
    slow.append(jit.branchTestPtr(CCallHelpers::NonZero, cell, TrustedImm32(1)));
    jit.load32(Address(T12, secret), T13);
    jit.xor32(Address(cell, FreeCell::offsetOfScrambledBits()), T13);
    jit.add64(T13, Address(T12, nextInterval));
    jit.load32(Address(T12, secret + 4), T13);
    jit.xor32(Address(cell, FreeCell::offsetOfScrambledBits() + 4), T13);
    jit.storePtr(cell, Address(T12, intervalStart));
    jit.addPtr(cell, T13);
    jit.storePtr(T13, Address(T12, intervalEnd));
#endif
    jit.jump().linkTo(bump, &jit);

    slow.link(&jit);
#if CPU(ARM64)
    // With the frames of a cold operation, which this was at the site.
    jit.move(T9, T12);
    jit.move(TrustedImm32(static_cast<unsigned>(Entry::operationAOTTypedAllocSlow) * sizeof(void*)), T9);
    generateColdOperation(jit, isLeaf, true, false, true, false, true, std::nullopt, T12);
#else
    preservingRegistersOfCaller(jit, true, [&](unsigned) {
        jit.move(T9, A1);
        jit.move(instanceGPR, A0);
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T11);
        jit.loadPtr(Address(T11, static_cast<unsigned>(Entry::operationAOTTypedAllocSlow) * sizeof(void*)), T11);
        jit.call(T11, OperationPtrTag);
    });
    jit.ret();
#endif
}
static void decodeCell32(CCallHelpers& jit, GPRReg word, GPRReg result)
{
#if CPU(ARM64)
    jit.addLeftShift64(cellCageBaseGPR, word, TrustedImm32(Typed::cell32Shift), result);
#else
    ASSERT_UNUSED(result, word == result);
    jit.lshift64(TrustedImm32(Typed::cell32Shift), word);
    jit.add64(Address(instanceGPR, Instance::offsetOfCellCageBase()), word);
#endif
}

static void makeLazyConstant(CCallHelpers& jit, bool isLeaf, Entry fill)
{
#if CPU(ARM64)
    jit.move(T9, T12);
    jit.move(TrustedImm32(static_cast<unsigned>(fill) * sizeof(void*)), T9);
    generateColdOperation(jit, isLeaf, true, false, true, false, true, std::nullopt, T12);
#else
    UNUSED_PARAM(isLeaf);
    preservingRegistersOfCaller(jit, true, [&](unsigned) {
        jit.move(T9, A1);
        jit.move(instanceGPR, A0);
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T11);
        jit.loadPtr(Address(T11, static_cast<unsigned>(fill) * sizeof(void*)), T11);
        jit.call(T11, OperationPtrTag);
    });
    jit.ret();
#endif
}

// T9: the index into a table of the instance whose entries are filled the first time they are asked for. Yields the entry.
static void generateLazyConstant(CCallHelpers& jit, bool isLeaf, ptrdiff_t table, Entry fill, unsigned block = 0)
{
    constexpr GPRReg result = GPRInfo::returnValueGPR;
    Jump isUninitialized;
#if CPU(ARM64)
    if (block)
        jit.m_assembler.movk<32>(T9, block, 16);
#else
    UNUSED_PARAM(block);
#endif
    jit.loadPtr(Address(instanceGPR, table), T11);
    if (table == Instance::offsetOfTypedStrings()) {
        // T9: the key. See Typed::startInStringTable().
        static_assert(noOverlap(result, T9, T10, T11, T12, T13, T14));
        jit.load32(Address(T11, Typed::offsetOfMaskInStringTable), T13);
#if CPU(ARM64)
        jit.addLeftShift32(T9, T9, TrustedImm32(Typed::stringTableShift), T12);
#else
        jit.mul32(TrustedImm32((1 << Typed::stringTableShift) + 1), T9, T12);
#endif
        jit.and32(T13, T12);
        jit.load64(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesEight), result);
        Jump isFound = jit.branch32(CCallHelpers::Equal, result, T9);
        jit.move(TrustedImm32(0), T14);
        auto probe = jit.label();
        Jump isFree = jit.branchTest64(CCallHelpers::Zero, result);
        jit.add32(TrustedImm32(1), T14);
        jit.add32(T14, T12);
        jit.and32(T13, T12);
        jit.load64(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesEight), result);
        jit.branch32(CCallHelpers::NotEqual, result, T9).linkTo(probe, &jit);
        isFound.link(&jit);
        jit.urshift64(TrustedImm32(32), result);
        if (Options::useAOTWideReferences()) {
            jit.loadPtr(Address(instanceGPR, Instance::offsetOfTypedWideStrings()), T11);
            jit.load64(CCallHelpers::BaseIndex(T11, result, CCallHelpers::TimesEight), result);
        } else {
            decodeCell32(jit, result, result);
        }
        returnToTypedCaller(jit, isLeaf);
        isFree.link(&jit);
        jit.sub32(TrustedImm32(1), T9);
        isUninitialized = jit.jump();
    } else if (table == Instance::offsetOfTypedClosures() && !Options::useAOTWideReferences()) {
        jit.zeroExtend32ToWord(T9, T12);
        jit.load32(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesFour), result);
        isUninitialized = jit.branchTest32(CCallHelpers::Zero, result);
        jit.add32(TrustedImm32(1), result);
        decodeCell32(jit, result, result);
        returnToTypedCaller(jit, isLeaf);
    } else {
        jit.zeroExtend32ToWord(T9, T12);
        jit.load64(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesEight), result);
        isUninitialized = jit.branchTest64(CCallHelpers::Zero, result);
        returnToTypedCaller(jit, isLeaf);
    }

    isUninitialized.link(&jit);
    makeLazyConstant(jit, isLeaf, fill);
}
static void generateCallModuleFunction(CCallHelpers& jit)
{
    static_assert(ClosureWord::numberOfOffsetBits == 32);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfTypedFunctions()), T11);
    jit.load32(CCallHelpers::BaseIndex(T11, A1, CCallHelpers::TimesEight), T11);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfCode()), T9);
    if constexpr (ClosureWord::offsetUnit == 4)
        jit.lshift64(TrustedImm32(2), T11);
    jit.add64(T9, T11);
    jit.farJump(T11, JSEntryPtrTag);
}
static void generateTypedStringMadeOnce(CCallHelpers& jit) { makeLazyConstant(jit, false, Entry::operationAOTTypedStringMadeOnce); }
#if CPU(ARM64)
static void generateLeafTypedStringMadeOnce(CCallHelpers& jit) { makeLazyConstant(jit, true, Entry::operationAOTTypedStringMadeOnce); }
#else
static void generateLeafTypedStringMadeOnce(CCallHelpers& jit) { jit.breakpoint(); }
#endif
#if CPU(ARM64)
#define AOT_GENERATE_LAZY_CONSTANT(name, table, fill) \
static void generate##name(CCallHelpers& jit) { generateLazyConstant(jit, false, table, fill); } \
static void generateLeaf##name(CCallHelpers& jit) { generateLazyConstant(jit, true, table, fill); }
#else
#define AOT_GENERATE_LAZY_CONSTANT(name, table, fill) \
static void generate##name(CCallHelpers& jit) { generateLazyConstant(jit, false, table, fill); } \
static void generateLeaf##name(CCallHelpers& jit) { jit.breakpoint(); }
#endif
AOT_GENERATE_LAZY_CONSTANT(TypedString, Instance::offsetOfTypedStrings(), Entry::operationAOTTypedStringConstant)
AOT_GENERATE_LAZY_CONSTANT(TypedClosure, Instance::offsetOfTypedClosures(), Entry::operationAOTTypedClosureConstant)
AOT_GENERATE_LAZY_CONSTANT(TypedBuiltinCell, Instance::offsetOfTypedBuiltins(), Entry::operationAOTTypedBuiltinCell)
#undef AOT_GENERATE_LAZY_CONSTANT
#if CPU(ARM64)
static void generateTypedStringMadeOnceInBlock(CCallHelpers& jit, bool isLeaf, unsigned block)
{
    jit.m_assembler.movk<32>(T9, block, 16);
    makeLazyConstant(jit, isLeaf, Entry::operationAOTTypedStringMadeOnce);
}
#define AOT_GENERATE_TYPED_STRING_IN_BLOCK(block) \
static void generateTypedStringInBlock##block(CCallHelpers& jit) { generateLazyConstant(jit, false, Instance::offsetOfTypedStrings(), Entry::operationAOTTypedStringConstant, block); } \
static void generateLeafTypedStringInBlock##block(CCallHelpers& jit) { generateLazyConstant(jit, true, Instance::offsetOfTypedStrings(), Entry::operationAOTTypedStringConstant, block); } \
static void generateTypedStringMadeOnceInBlock##block(CCallHelpers& jit) { generateTypedStringMadeOnceInBlock(jit, false, block); } \
static void generateLeafTypedStringMadeOnceInBlock##block(CCallHelpers& jit) { generateTypedStringMadeOnceInBlock(jit, true, block); }
#else
#define AOT_GENERATE_TYPED_STRING_IN_BLOCK(block) \
static void generateTypedStringInBlock##block(CCallHelpers& jit) { jit.breakpoint(); } \
static void generateLeafTypedStringInBlock##block(CCallHelpers& jit) { jit.breakpoint(); } \
static void generateTypedStringMadeOnceInBlock##block(CCallHelpers& jit) { jit.breakpoint(); } \
static void generateLeafTypedStringMadeOnceInBlock##block(CCallHelpers& jit) { jit.breakpoint(); }
#endif
AOT_GENERATE_TYPED_STRING_IN_BLOCK(1)
AOT_GENERATE_TYPED_STRING_IN_BLOCK(2)
AOT_GENERATE_TYPED_STRING_IN_BLOCK(3)
AOT_GENERATE_TYPED_STRING_IN_BLOCK(4)
AOT_GENERATE_TYPED_STRING_IN_BLOCK(5)
AOT_GENERATE_TYPED_STRING_IN_BLOCK(6)
AOT_GENERATE_TYPED_STRING_IN_BLOCK(7)
#undef AOT_GENERATE_TYPED_STRING_IN_BLOCK

// A substring's cell in `string`: what it is a part of, which is flat, into `base`; where it starts there into `offset`.
static void loadBaseAndOffsetOfSubstring(CCallHelpers& jit, GPRReg string, GPRReg base, GPRReg offset)
{
    jit.load16(Address(string, JSRopeString::offsetOfFiber2()), offset);
    jit.load32(Address(string, JSRopeString::offsetOfFiber1Lower()), base);
#if CPU(ARM64)
    jit.orLeftShift64(base, offset, TrustedImm32(32), base);
#else
    jit.lshift64(TrustedImm32(32), offset);
    jit.or64(offset, base);
#endif
    jit.load64(Address(string, JSRopeString::offsetOfFiber2()), offset);
    jit.urshift64(TrustedImm32(16), offset);
}

void emitCharactersOfSubstring(CCallHelpers& jit)
{
    constexpr GPRReg base = T9;
    constexpr GPRReg flags = T9;
    constexpr GPRReg offset = T10;
    constexpr GPRReg string = T12;
    constexpr GPRReg data = T12;
    constexpr GPRReg length = T13;
    static_assert(noOverlap(T9, T10, T12, T13));
    jit.load32(Address(string, JSRopeString::offsetOfLength()), length);
    loadBaseAndOffsetOfSubstring(jit, string, base, offset);
    jit.loadPtr(Address(base, JSString::offsetOfValue()), base);
    jit.loadPtr(Address(base, StringImpl::dataOffset()), data);
    jit.load32(Address(base, StringImpl::flagsOffset()), flags);
    jit.and32(TrustedImm32(StringImpl::flagIs8Bit()), flags);
    Jump isNarrow = jit.branchTest32(CCallHelpers::NonZero, flags);
    jit.lshift64(TrustedImm32(1), offset);
    isNarrow.link(&jit);
    jit.addPtr(offset, data);
}

static void generateCodeUnitOfRope(CCallHelpers& jit, bool isLeaf)
{
    constexpr GPRReg result = GPRInfo::returnValueGPR;
    constexpr GPRReg fiber = T9;
    constexpr GPRReg place = T11;
    static_assert(noOverlap(result, fiber, place, A1, A2));

    CCallHelpers::JumpList isOutside;
    jit.loadPtr(Address(A1, JSString::offsetOfValue()), fiber);
    Jump isFlat = jit.branchTestPtr(CCallHelpers::Zero, fiber, TrustedImm32(JSString::isRopeInPointer));
    Jump isNoSubstring = jit.branchTestPtr(CCallHelpers::Zero, fiber, TrustedImm32(JSRopeString::isSubstringInPointer));
    jit.load32(Address(A1, JSRopeString::offsetOfLength()), result);
    isOutside.append(jit.branch64(CCallHelpers::AboveOrEqual, A2, result));
    loadBaseAndOffsetOfSubstring(jit, A1, fiber, place);
    jit.add64(A2, place);
    jit.loadPtr(Address(fiber, JSString::offsetOfValue()), fiber);
    Jump isPlaced = jit.jump();

    isFlat.link(&jit);
    jit.load32(Address(fiber, StringImpl::lengthMemoryOffset()), result);
    isOutside.append(jit.branch64(CCallHelpers::AboveOrEqual, A2, result));
    jit.move(A2, place);

    isPlaced.link(&jit);
    jit.load32(Address(fiber, StringImpl::flagsOffset()), result);
    jit.loadPtr(Address(fiber, StringImpl::dataOffset()), fiber);
    Jump isWide = jit.branchTest32(CCallHelpers::Zero, result, TrustedImm32(StringImpl::flagIs8Bit()));
    jit.load8(CCallHelpers::BaseIndex(fiber, place, CCallHelpers::TimesOne), result);
    returnToTypedCaller(jit, isLeaf);
    isWide.link(&jit);
    jit.load16(CCallHelpers::BaseIndex(fiber, place, CCallHelpers::TimesTwo), result);
    returnToTypedCaller(jit, isLeaf);

    isOutside.link(&jit);
    jit.move(CCallHelpers::TrustedImm64(-1), result);
    returnToTypedCaller(jit, isLeaf);

    isNoSubstring.link(&jit);
    jit.move(TrustedImm32(static_cast<unsigned>(Entry::operationAOTTypedCodeUnitOfRope) * sizeof(void*)), T9);
    generateColdOperation(jit, isLeaf, true);
}
static void generateCodeUnitOfRope(CCallHelpers& jit) { generateCodeUnitOfRope(jit, false); }
#if CPU(ARM64)
static void generateLeafCodeUnitOfRope(CCallHelpers& jit) { generateCodeUnitOfRope(jit, true); }
#else
static void generateLeafCodeUnitOfRope(CCallHelpers& jit) { jit.breakpoint(); }
#endif

static void generateCharactersOfSubstring(CCallHelpers& jit)
{
    emitCharactersOfSubstring(jit);
    jit.ret();
}

// `fiber`: what the string in `string` points to; then its characters, if they have 8 bits and it is flat or a part of a flat one: what slice, substring and trim yield. `string` is kept.
static void loadCharacters8(CCallHelpers& jit, GPRReg string, GPRReg fiber, GPRReg scratch, CCallHelpers::JumpList& isLeftToOperation)
{
    Jump isRope = jit.branchIfRopeStringImpl(fiber);
    isLeftToOperation.append(jit.branchTest32(CCallHelpers::Zero, Address(fiber, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.loadPtr(Address(fiber, StringImpl::dataOffset()), fiber);
    Jump isLoaded = jit.jump();
    isRope.link(&jit);
    isLeftToOperation.append(jit.branchTestPtr(CCallHelpers::Zero, fiber, TrustedImm32(JSRopeString::isSubstringInPointer)));
    loadBaseAndOffsetOfSubstring(jit, string, fiber, scratch);
    jit.loadPtr(Address(fiber, JSString::offsetOfValue()), fiber);
    isLeftToOperation.append(jit.branchTest32(CCallHelpers::Zero, Address(fiber, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.loadPtr(Address(fiber, StringImpl::dataOffset()), fiber);
    jit.addPtr(scratch, fiber);
    isLoaded.link(&jit);
}

// T12, T13: bytes. The result register: how many, not none. Returns to the caller of the stub with 1 or 0 there. `isNot`: whoever has found out before.
static void compareBytesAndReturn(CCallHelpers& jit, bool isLeaf, CCallHelpers::JumpList& isNot)
{
    constexpr GPRReg result = GPRInfo::returnValueGPR;
    using BaseIndex = CCallHelpers::BaseIndex;
    CCallHelpers::JumpList lastAreLoaded;
    // T9 and T11 hold what is compared.
    Jump isBelow8 = jit.branch32(CCallHelpers::Below, result, TrustedImm32(8));
    // Words, and the last one once more with those before it.
    jit.sub32(TrustedImm32(8), result);
    jit.move(TrustedImm32(0), T14);
    auto nextWord = jit.label();
    jit.load64(BaseIndex(T12, T14, CCallHelpers::TimesOne), T9);
    jit.load64(BaseIndex(T13, T14, CCallHelpers::TimesOne), T11);
    isNot.append(jit.branch64(CCallHelpers::NotEqual, T9, T11));
    jit.add32(TrustedImm32(8), T14);
    jit.branch32(CCallHelpers::Below, T14, result).linkTo(nextWord, &jit);
    jit.load64(BaseIndex(T12, result, CCallHelpers::TimesOne), T9);
    jit.load64(BaseIndex(T13, result, CCallHelpers::TimesOne), T11);
    lastAreLoaded.append(jit.jump());

    isBelow8.link(&jit);
    Jump isBelow4 = jit.branch32(CCallHelpers::Below, result, TrustedImm32(4));
    jit.load32(Address(T12), T9);
    jit.load32(Address(T13), T11);
    isNot.append(jit.branch32(CCallHelpers::NotEqual, T9, T11));
    jit.sub32(TrustedImm32(4), result);
    jit.load32(BaseIndex(T12, result, CCallHelpers::TimesOne), T9);
    jit.load32(BaseIndex(T13, result, CCallHelpers::TimesOne), T11);
    lastAreLoaded.append(jit.jump());

    isBelow4.link(&jit);
    Jump isOne = jit.branch32(CCallHelpers::Below, result, TrustedImm32(2));
    jit.load16(Address(T12), T9);
    jit.load16(Address(T13), T11);
    isNot.append(jit.branch32(CCallHelpers::NotEqual, T9, T11));
    jit.sub32(TrustedImm32(2), result);
    jit.load16(BaseIndex(T12, result, CCallHelpers::TimesOne), T9);
    jit.load16(BaseIndex(T13, result, CCallHelpers::TimesOne), T11);
    lastAreLoaded.append(jit.jump());

    isOne.link(&jit);
    jit.load8(Address(T12), T9);
    jit.load8(Address(T13), T11);

    lastAreLoaded.link(&jit);
    jit.compare64(CCallHelpers::Equal, T9, T11, result);
    returnToTypedCaller(jit, isLeaf);

    isNot.link(&jit);
    jit.move(TrustedImm32(0), result);
    returnToTypedCaller(jit, isLeaf);
}

// T9: the number of a text of the program, which is all ASCII and not empty. T11: a string. Yields whether the string is the text: 1 or 0. No cell is made for the text: the characters
// are compared with the bytes in the image. Every register is kept but the temporaries.
static void generateStringIsText(CCallHelpers& jit, bool isLeaf)
{
    constexpr GPRReg result = GPRInfo::returnValueGPR;
    static_assert(noOverlap(result, T9, T10, T11, T12, T13, T14));
    using BaseIndex = CCallHelpers::BaseIndex;
    CCallHelpers::JumpList isLeftToOperation;
    CCallHelpers::JumpList isNot;
    jit.loadPtr(Address(T11, JSString::offsetOfValue()), T12);
    Jump isRope = jit.branchTestPtr(CCallHelpers::NonZero, T12, TrustedImm32(JSString::isRopeInPointer));
    // Pairs of offset and length.
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfTypedTexts()), T14);
    jit.load32(BaseIndex(T14, T9, CCallHelpers::TimesEight, sizeof(uint32_t)), result);
    jit.load32(BaseIndex(T14, T9, CCallHelpers::TimesEight), T14);
    isNot.append(jit.branch32(CCallHelpers::NotEqual, Address(T12, StringImpl::lengthMemoryOffset()), result));
    isLeftToOperation.append(jit.branchTest32(CCallHelpers::Zero, Address(T12, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.loadPtr(Address(T12, StringImpl::dataOffset()), T12);
    CCallHelpers::Label hasCharacters = jit.label();
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfTypedData()), T13);
    jit.addPtr(T14, T13);

    compareBytesAndReturn(jit, isLeaf, isNot);

    isRope.link(&jit);
    isLeftToOperation.append(jit.branchTestPtr(CCallHelpers::Zero, T12, TrustedImm32(JSRopeString::isSubstringInPointer)));
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfTypedTexts()), T14);
    jit.load32(BaseIndex(T14, T9, CCallHelpers::TimesEight, sizeof(uint32_t)), result);
    Jump isNotAsLong = jit.branch32(CCallHelpers::NotEqual, Address(T11, JSRopeString::offsetOfLength()), result);
    jit.load32(BaseIndex(T14, T9, CCallHelpers::TimesEight), T14);
    loadBaseAndOffsetOfSubstring(jit, T11, T12, T13);
    jit.loadPtr(Address(T12, JSString::offsetOfValue()), T12);
    isLeftToOperation.append(jit.branchTest32(CCallHelpers::Zero, Address(T12, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.loadPtr(Address(T12, StringImpl::dataOffset()), T12);
    jit.addPtr(T13, T12);
    jit.jump().linkTo(hasCharacters, &jit);
    isNotAsLong.link(&jit);
    jit.move(TrustedImm32(0), result);
    returnToTypedCaller(jit, isLeaf);

    isLeftToOperation.link(&jit);
#if CPU(ARM64)
    jit.move(T9, T12);
    jit.move(TrustedImm32(static_cast<unsigned>(Entry::operationAOTTypedStringIsText) * sizeof(void*)), T9);
    generateColdOperation(jit, isLeaf, true, false, true, false, true, std::nullopt, T11, T12);
#else
    preservingRegistersOfCaller(jit, true, [&](unsigned) {
        static_assert(noOverlap(T9, T11, A1) && noOverlap(T9, A2));
        jit.move(T11, A1);
        jit.move(T9, A2);
        jit.move(instanceGPR, A0);
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T11);
        jit.loadPtr(Address(T11, static_cast<unsigned>(Entry::operationAOTTypedStringIsText) * sizeof(void*)), T11);
        jit.call(T11, OperationPtrTag);
    });
    jit.ret();
#endif
}
static void generateStringIsText(CCallHelpers& jit) { generateStringIsText(jit, false); }
#if CPU(ARM64)
static void generateLeafStringIsText(CCallHelpers& jit) { generateStringIsText(jit, true); }
#else
static void generateLeafStringIsText(CCallHelpers& jit) { jit.breakpoint(); }
#endif

// T9: the number of a text of the program, which is all ASCII and not empty. T11: a string. Yields whether the string starts, or ends, with the text: 1 or 0. As generateStringIsText().
static void generateStringHasTextAtOneEnd(CCallHelpers& jit, bool isLeaf, bool isAtEnd)
{
    constexpr GPRReg result = GPRInfo::returnValueGPR;
    static_assert(noOverlap(result, T9, T10, T11, T12, T13, T14));
    using BaseIndex = CCallHelpers::BaseIndex;
    CCallHelpers::JumpList isLeftToOperation;
    CCallHelpers::JumpList isNot;
    jit.loadPtr(Address(T11, JSString::offsetOfValue()), T12);
    isLeftToOperation.append(jit.branchTestPtr(CCallHelpers::NonZero, T12, TrustedImm32(JSString::isRopeInPointer)));
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfTypedTexts()), T14);
    jit.load32(BaseIndex(T14, T9, CCallHelpers::TimesEight, sizeof(uint32_t)), result);
    jit.load32(BaseIndex(T14, T9, CCallHelpers::TimesEight), T14);
    jit.load32(Address(T12, StringImpl::lengthMemoryOffset()), T13);
    isNot.append(jit.branch32(CCallHelpers::Below, T13, result));
    isLeftToOperation.append(jit.branchTest32(CCallHelpers::Zero, Address(T12, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.loadPtr(Address(T12, StringImpl::dataOffset()), T12);
    if (isAtEnd) {
        jit.sub32(result, T13);
        jit.addPtr(T13, T12);
    }
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfTypedData()), T13);
    jit.addPtr(T14, T13);
    compareBytesAndReturn(jit, isLeaf, isNot);

    isLeftToOperation.link(&jit);
    Entry operation = isAtEnd ? Entry::operationAOTTypedStringEndsWithText : Entry::operationAOTTypedStringStartsWithText;
#if CPU(ARM64)
    jit.move(T9, T12);
    jit.move(TrustedImm32(static_cast<unsigned>(operation) * sizeof(void*)), T9);
    generateColdOperation(jit, isLeaf, true, true, true, false, true, std::nullopt, T11, T12);
#else
    // As leaveToOperationOnString(): the site clobbers like a call here.
    static_assert(noOverlap(T9, T11, A1) && noOverlap(T9, A2));
    jit.move(T11, A1);
    jit.move(T9, A2);
    jit.move(TrustedImm32(static_cast<unsigned>(operation) * sizeof(void*)), T9);
    generateColdOperation(jit, false, true);
#endif
}
static void generateStringStartsWithText(CCallHelpers& jit) { generateStringHasTextAtOneEnd(jit, false, false); }
static void generateStringEndsWithText(CCallHelpers& jit) { generateStringHasTextAtOneEnd(jit, false, true); }
#if CPU(ARM64)
static void generateLeafStringStartsWithText(CCallHelpers& jit) { generateStringHasTextAtOneEnd(jit, true, false); }
static void generateLeafStringEndsWithText(CCallHelpers& jit) { generateStringHasTextAtOneEnd(jit, true, true); }
#else
static void generateLeafStringStartsWithText(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafStringEndsWithText(CCallHelpers& jit) { jit.breakpoint(); }
#endif

// T11, T12: two strings, not one cell, as long as each other: that much the site has seen. Yields whether they have the same characters: 1 or 0. Every register is kept but the temporaries.
static void generateStringsAreEqual(CCallHelpers& jit, bool isLeaf)
{
    constexpr GPRReg result = GPRInfo::returnValueGPR;
    static_assert(noOverlap(result, T9, T10, T11, T12, T13, T14));
    CCallHelpers::JumpList isLeftToOperation;
    CCallHelpers::JumpList is;
    CCallHelpers::JumpList isNot;
    jit.loadPtr(Address(T11, JSString::offsetOfValue()), T13);
    jit.loadPtr(Address(T12, JSString::offsetOfValue()), T14);
    jit.or64(T13, T14, T9);
    Jump oneIsRope = jit.branchTestPtr(CCallHelpers::NonZero, T9, TrustedImm32(JSString::isRopeInPointer));
    is.append(jit.branchPtr(CCallHelpers::Equal, T13, T14));
    jit.load32(Address(T13, StringImpl::lengthMemoryOffset()), result);
    is.append(jit.branchTest32(CCallHelpers::Zero, result));
    jit.load32(Address(T13, StringImpl::flagsOffset()), T9);
    jit.and32(Address(T14, StringImpl::flagsOffset()), T9);
    // There is one atom of a text.
    isNot.append(jit.branchTest32(CCallHelpers::NonZero, T9, TrustedImm32(StringImpl::flagIsAtom())));
    Jump areNotBoth8Bit = jit.branchTest32(CCallHelpers::Zero, T9, TrustedImm32(StringImpl::flagIs8Bit()));
    auto loadCharacters = jit.label();
    jit.loadPtr(Address(T13, StringImpl::dataOffset()), T12);
    jit.loadPtr(Address(T14, StringImpl::dataOffset()), T13);
    auto compare = jit.label();
    compareBytesAndReturn(jit, isLeaf, isNot);

    // Both of 16 bits: twice as many bytes.
    areNotBoth8Bit.link(&jit);
    isLeftToOperation.append(jit.branchTest32(CCallHelpers::NonZero, Address(T13, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    isLeftToOperation.append(jit.branchTest32(CCallHelpers::NonZero, Address(T14, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.lshift32(TrustedImm32(1), result);
    jit.jump().linkTo(loadCharacters, &jit);

    // The length, wherever the first one keeps it. It is not 0: no rope is empty, and the other string is as long.
    oneIsRope.link(&jit);
    Jump firstIsRope = jit.branchIfRopeStringImpl(T13);
    jit.load32(Address(T13, StringImpl::lengthMemoryOffset()), result);
    Jump isLoaded = jit.jump();
    firstIsRope.link(&jit);
    jit.load32(Address(T11, JSRopeString::offsetOfLength()), result);
    isLoaded.link(&jit);
    loadCharacters8(jit, T11, T13, T9, isLeftToOperation);
    loadCharacters8(jit, T12, T14, T9, isLeftToOperation);
    jit.move(T14, T12);
    jit.jump().linkTo(compare, &jit);

    is.link(&jit);
    jit.move(TrustedImm32(1), result);
    returnToTypedCaller(jit, isLeaf);

    isLeftToOperation.link(&jit);
#if CPU(ARM64)
    jit.move(TrustedImm32(static_cast<unsigned>(Entry::operationAOTTypedStringsAreEqual) * sizeof(void*)), T9);
    generateColdOperation(jit, isLeaf, true, false, true, false, true, std::nullopt, T11, T12);
#else
    preservingRegistersOfCaller(jit, true, [&](unsigned) {
        static_assert(T12 == A0 && noOverlap(T11, T12, A1, A2));
        jit.move(T12, A2);
        jit.move(T11, A1);
        jit.move(instanceGPR, A0);
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T11);
        jit.loadPtr(Address(T11, static_cast<unsigned>(Entry::operationAOTTypedStringsAreEqual) * sizeof(void*)), T11);
        jit.call(T11, OperationPtrTag);
    });
    jit.ret();
#endif
}
static void generateStringsAreEqual(CCallHelpers& jit) { generateStringsAreEqual(jit, false); }
#if CPU(ARM64)
static void generateLeafStringsAreEqual(CCallHelpers& jit) { generateStringsAreEqual(jit, true); }
#else
static void generateLeafStringsAreEqual(CCallHelpers& jit) { jit.breakpoint(); }
#endif

static void makePartOfFlatStringAndReturn(CCallHelpers& jit, bool isLeaf, CCallHelpers::JumpList& isLeftToOperation, CCallHelpers::Label& allocate, bool isKnownToBe8Bit = false)
{
    constexpr GPRReg result = GPRInfo::returnValueGPR;
    static_assert(noOverlap(result, T9, T10, T11, T12, T13, T14));
    static_assert(StringImpl::flagIs8Bit() == JSRopeString::is8BitInPointer);
    static_assert(JSRopeString::offsetOfFiber0() == sizeof(EncodedJSValue) && JSRopeString::offsetOfLength() == 2 * sizeof(EncodedJSValue));
    Jump isAtMostOne = jit.branch32(CCallHelpers::LessThanOrEqual, T13, TrustedImm32(1));
    Jump isWhole = jit.branch32(CCallHelpers::Equal, Address(T14, StringImpl::lengthMemoryOffset()), T13);
#if CPU(ARM64)
    static_assert(noOverlap(T15, result, T9, T10, T11, T12, T13, T14));
    if (!isKnownToBe8Bit)
        jit.load32(Address(T14, StringImpl::flagsOffset()), T15);
#endif
    allocate = jit.label();
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfRopeStringAllocator()), T9);
    isLeftToOperation.append(jit.branchTestPtr(CCallHelpers::Zero, T9));
    jit.emitAllocateWithNonNullAllocator(result, JITAllocator::variable(), T9, T14, isLeftToOperation, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
    jit.load32(Address(instanceGPR, Instance::offsetOfStringStructureID()), T9);
    jit.or64(CCallHelpers::TrustedImm64(static_cast<uint64_t>(TypeInfoBlob(NonArray, TypeInfo(StringType, JSString::StructureFlags)).blob()) << 32), T9);
#if CPU(ARM64)
    if (isKnownToBe8Bit)
        jit.move(TrustedImm32(JSString::isRopeInPointer | JSRopeString::isSubstringInPointer | JSRopeString::is8BitInPointer), T14);
    else {
        jit.and32(TrustedImm32(JSRopeString::is8BitInPointer), T15, T14);
        jit.or64(TrustedImm32(JSString::isRopeInPointer | JSRopeString::isSubstringInPointer), T14);
    }
    jit.storePair64(T9, T14, result, TrustedImm32(0));
    jit.orLeftShift64(T13, T11, TrustedImm32(32), T9);
    jit.urshift64(TrustedImm32(32), T11);
    jit.orLeftShift64(T11, T12, TrustedImm32(16), T11);
#else
    if (isKnownToBe8Bit)
        jit.move(TrustedImm32(JSString::isRopeInPointer | JSRopeString::isSubstringInPointer | JSRopeString::is8BitInPointer), T14);
    else {
        jit.loadPtr(Address(T11, JSString::offsetOfValue()), T14);
        jit.load32(Address(T14, StringImpl::flagsOffset()), T14);
        jit.and32(TrustedImm32(JSRopeString::is8BitInPointer), T14);
        jit.or64(TrustedImm32(JSString::isRopeInPointer | JSRopeString::isSubstringInPointer), T14);
    }
    jit.storePair64(T9, T14, result, TrustedImm32(0));
    jit.move(T11, T9);
    jit.lshift64(TrustedImm32(32), T9);
    jit.or64(T13, T9);
    jit.urshift64(TrustedImm32(32), T11);
    jit.lshift64(TrustedImm32(16), T12);
    jit.or64(T12, T11);
#endif
    jit.storePair64(T9, T11, result, TrustedImm32(2 * sizeof(EncodedJSValue)));
#if CPU(ARM64)
    Jump isNotFenced = jit.branchTest8(CCallHelpers::Zero, Address(instanceGPR, Instance::offsetOfMutatorShouldBeFenced()));
    jit.storeFence();
    isNotFenced.link(&jit);
#endif
    returnToTypedCaller(jit, isLeaf);

    isWhole.link(&jit);
    jit.move(T11, result);
    returnToTypedCaller(jit, isLeaf);

    isAtMostOne.link(&jit);
    Jump isNone = jit.branch32(CCallHelpers::LessThan, T13, TrustedImm32(1));
    if (!isKnownToBe8Bit)
        isLeftToOperation.append(jit.branchTest32(CCallHelpers::Zero, Address(T14, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    static_assert(maxSingleCharacterString == std::numeric_limits<Latin1Character>::max());
    jit.loadPtr(Address(T14, StringImpl::dataOffset()), T14);
    jit.load8(CCallHelpers::BaseIndex(T14, T12, CCallHelpers::TimesOne), T14);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfSingleCharacterStrings()), T9);
    jit.loadPtr(CCallHelpers::BaseIndex(T9, T14, CCallHelpers::TimesEight), result);
    returnToTypedCaller(jit, isLeaf);

    isNone.link(&jit);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfEmptyString()), result);
    returnToTypedCaller(jit, isLeaf);
}

static void goOnWithBaseOfSubstring(CCallHelpers& jit, bool isLeaf, CCallHelpers::Label make, CCallHelpers::Label allocate)
{
    Jump isAllOfIt = jit.branch32(CCallHelpers::Equal, Address(T11, JSRopeString::offsetOfLength()), T13);
    loadBaseAndOffsetOfSubstring(jit, T11, T14, T9);
    jit.move(T14, T11);
    jit.add64(T9, T12);
    jit.loadPtr(Address(T11, JSString::offsetOfValue()), T14);
#if CPU(ARM64)
    jit.load32(Address(T14, StringImpl::flagsOffset()), T15);
#endif
    jit.branch32(CCallHelpers::GreaterThan, T13, TrustedImm32(1)).linkTo(allocate, &jit);
    jit.jump().linkTo(make, &jit);
    isAllOfIt.link(&jit);
    jit.move(T11, GPRInfo::returnValueGPR);
    returnToTypedCaller(jit, isLeaf);
}

static void generateHoldToType(CCallHelpers& jit, bool isLeaf)
{
    using Typed::RuntimeType;
    using Typed::RuntimeShapeTest;
    constexpr GPRReg result = GPRInfo::returnValueGPR;
    constexpr GPRReg value = A1;
    constexpr GPRReg number = A2;
    constexpr GPRReg type = T9;
    constexpr GPRReg scratch = T11;
    static_assert(noOverlap(result, value, number, type, scratch, instanceGPR));
    static_assert(sizeof(RuntimeType) == 8);
    CCallHelpers::JumpList fits;
    CCallHelpers::JumpList isLeftToOperation;

    jit.loadPtr(Address(instanceGPR, Instance::offsetOfTypedData()), result);
    jit.load32(Address(result, OBJECT_OFFSETOF(Typed::Data, typesOffset)), type);
    jit.addPtr(result, type);
    jit.load32(CCallHelpers::BaseIndex(type, number, CCallHelpers::TimesFour), type);
    jit.addPtr(result, type);

    isLeftToOperation.append(jit.branchTest64(CCallHelpers::Zero, value));
    Jump isNoCell = jit.branchIfNotCell(value);
    jit.load8(Address(value, JSCell::typeInfoTypeOffset()), scratch);
    Jump isString = jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(StringType));
    isLeftToOperation.append(jit.branch32(CCallHelpers::Below, scratch, TrustedImm32(ObjectType)));
    fits.append(jit.branchTest8(CCallHelpers::NonZero, Address(type, OBJECT_OFFSETOF(RuntimeType, bits)), TrustedImm32(RuntimeType::allowsAnyObjectBit | RuntimeType::admitsForeignObjectsBit)));
    jit.load16(Address(type, OBJECT_OFFSETOF(RuntimeType, viewsCount)), result);
    Jump hasNoView = jit.branchTest32(CCallHelpers::Zero, result);
    isLeftToOperation.append(jit.branch32(CCallHelpers::NotEqual, result, TrustedImm32(1)));
    jit.load32(Address(type, sizeof(RuntimeType)), result);
    jit.signExtend32ToPtr(result, result);
    jit.addPtr(type, result);
    isLeftToOperation.append(jit.branch32(CCallHelpers::NotEqual, Address(result, OBJECT_OFFSETOF(RuntimeShapeTest, set)), TrustedImm32(-1)));
    jit.load32(Address(result, OBJECT_OFFSETOF(RuntimeShapeTest, first)), scratch);
    jit.load32(Address(result, OBJECT_OFFSETOF(RuntimeShapeTest, count)), result);
    jit.add32(result, scratch);
    jit.sub32(TrustedImm32(1), scratch);
    jit.add32(Address(value, Typed::offsetOfShapeWord), scratch);
    fits.append(jit.branch32(CCallHelpers::Below, scratch, result));
    isLeftToOperation.append(jit.jump());

    hasNoView.link(&jit);
    isLeftToOperation.append(jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(ObjectType)));
    isLeftToOperation.append(jit.branchTest8(CCallHelpers::Zero, Address(type, OBJECT_OFFSETOF(RuntimeType, jsTypesCount)), TrustedImm32(0xff)));
    fits.append(jit.branch32(CCallHelpers::Equal, scratch, Address(type, sizeof(RuntimeType))));
    isLeftToOperation.append(jit.jump());

    isString.link(&jit);
    jit.load16(Address(type, OBJECT_OFFSETOF(RuntimeType, primitives)), scratch);
    fits.append(jit.branchTest32(CCallHelpers::NonZero, scratch, TrustedImm32(Typed::Primitive::String)));
    isLeftToOperation.append(jit.jump());

    isNoCell.link(&jit);
    jit.load16(Address(type, OBJECT_OFFSETOF(RuntimeType, primitives)), scratch);
    Jump isNotUndefined = jit.branch64(CCallHelpers::NotEqual, value, TrustedImm32(JSValue::ValueUndefined));
    fits.append(jit.branchTest32(CCallHelpers::NonZero, scratch, TrustedImm32(Typed::Primitive::Undefined)));
    isLeftToOperation.append(jit.jump());
    isNotUndefined.link(&jit);
    isLeftToOperation.append(jit.branch64(CCallHelpers::NotEqual, value, TrustedImm32(JSValue::ValueNull)));
    isLeftToOperation.append(jit.branchTest32(CCallHelpers::Zero, scratch, TrustedImm32(Typed::Primitive::Null)));

    fits.link(&jit);
    jit.move(value, result);
    returnToTypedCaller(jit, isLeaf);

    isLeftToOperation.link(&jit);
    jit.move(TrustedImm32(static_cast<unsigned>(Entry::operationAOTTypedHoldToType) * sizeof(void*)), T9);
    generateColdOperation(jit, isLeaf, true, true, true, false, true);
}
static void generateHoldToType(CCallHelpers& jit) { generateHoldToType(jit, false); }
#if CPU(ARM64)
static void generateLeafHoldToType(CCallHelpers& jit) { generateHoldToType(jit, true); }
#else
static void generateLeafHoldToType(CCallHelpers& jit) { jit.breakpoint(); }
#endif

static void leaveToOperationOnString(CCallHelpers& jit, bool isLeaf, Entry operation)
{
#if CPU(ARM64)
    jit.move(TrustedImm32(static_cast<unsigned>(operation) * sizeof(void*)), T9);
    generateColdOperation(jit, isLeaf, true, true, true, false, false, std::nullopt, T11, T12);
#else
    UNUSED_PARAM(isLeaf);
    jit.move(TrustedImm32(static_cast<unsigned>(operation) * sizeof(void*)), T9);
    static_assert(T12 == A0 && noOverlap(T11, T12, A1, A2));
    jit.move(T12, A2);
    jit.move(T11, A1);
    generateColdOperation(jit, false, true, true, true, false, false);
#endif
}

static void generateStringTrim(CCallHelpers& jit, bool isLeaf, bool isFlat8, unsigned ends)
{
    constexpr GPRReg result = GPRInfo::returnValueGPR;
    static_assert(noOverlap(result, T9, T10, T11, T12, T13, T14));
    CCallHelpers::JumpList isLeftToOperation;
    CCallHelpers::JumpList isLeftToOperationOnPart;
    auto trim = [&](bool is16Bit) {
        CCallHelpers::JumpList isTrimmed;
        auto load = [&](GPRReg index, int32_t offset) {
            if (is16Bit)
                jit.load16(CCallHelpers::BaseIndex(result, index, CCallHelpers::TimesTwo, offset * 2), T14);
            else
                jit.load8(CCallHelpers::BaseIndex(result, index, CCallHelpers::TimesOne, offset), T14);
        };
        auto branchIfNotWhiteSpace = [&] {
            CCallHelpers::JumpList isNot;
            CCallHelpers::JumpList is;
            is.append(jit.branch32(CCallHelpers::Equal, T14, TrustedImm32(' ')));
            is.append(jit.branch32(CCallHelpers::Equal, T14, TrustedImm32(0xa0)));
            if (is16Bit) {
                Jump isLow = jit.branch32(CCallHelpers::Below, T14, TrustedImm32(0x1680));
                is.append(jit.branch32(CCallHelpers::Equal, T14, TrustedImm32(0x1680)));
                isNot.append(jit.branch32(CCallHelpers::Below, T14, TrustedImm32(0x2000)));
                is.append(jit.branch32(CCallHelpers::BelowOrEqual, T14, TrustedImm32(0x200a)));
                for (int32_t character : { 0x2028, 0x2029, 0x202f, 0x205f, 0x3000, 0xfeff })
                    is.append(jit.branch32(CCallHelpers::Equal, T14, TrustedImm32(character)));
                isNot.append(jit.jump());
                isLow.link(&jit);
            }
            jit.sub32(TrustedImm32('\t'), T14);
            isNot.append(jit.branch32(CCallHelpers::Above, T14, TrustedImm32('\r' - '\t')));
            is.link(&jit);
            return isNot;
        };
        jit.move(TrustedImm32(0), T12);
        if (ends & 1) {
            auto nextAtStart = jit.label();
            Jump isAllWhiteSpace = jit.branch32(CCallHelpers::AboveOrEqual, T12, T13);
            load(T12, 0);
            auto startIsFound = branchIfNotWhiteSpace();
            jit.add32(TrustedImm32(1), T12);
            jit.jump().linkTo(nextAtStart, &jit);
            isAllWhiteSpace.link(&jit);
            startIsFound.link(&jit);
        }
        if (ends & 2) {
            auto nextAtEnd = jit.label();
            isTrimmed.append(jit.branch32(CCallHelpers::BelowOrEqual, T13, T12));
            load(T13, -1);
            isTrimmed.append(branchIfNotWhiteSpace());
            jit.sub32(TrustedImm32(1), T13);
            jit.jump().linkTo(nextAtEnd, &jit);
        }
        return isTrimmed;
    };

    if (isFlat8) {
        jit.move(T12, result);
        trim(false).link(&jit);
        jit.sub32(T12, T13);
        jit.loadPtr(Address(T11, JSString::offsetOfValue()), T14);
        CCallHelpers::Label allocate;
        makePartOfFlatStringAndReturn(jit, isLeaf, isLeftToOperationOnPart, allocate, true);
        isLeftToOperationOnPart.link(&jit);
        jit.lshift64(TrustedImm32(32), T13);
        jit.or64(T13, T12);
        leaveToOperationOnString(jit, isLeaf, Entry::operationAOTTypedStringPart);
        return;
    }
    jit.loadPtr(Address(T11, JSString::offsetOfValue()), T14);
    Jump isRope = jit.branchIfRopeStringImpl(T14);
    jit.loadPtr(Address(T14, StringImpl::dataOffset()), result);
    jit.load32(Address(T14, StringImpl::lengthMemoryOffset()), T13);
    Jump is16Bit = jit.branchTest32(CCallHelpers::Zero, Address(T14, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit()));
    auto trim8 = jit.label();
    trim(false).link(&jit);

    auto isTrimmed = jit.label();
    jit.sub32(T12, T13);
    jit.loadPtr(Address(T11, JSString::offsetOfValue()), T14);
    Jump wasRope = jit.branchIfRopeStringImpl(T14);
    auto make = jit.label();
    CCallHelpers::Label allocate;
    makePartOfFlatStringAndReturn(jit, isLeaf, isLeftToOperationOnPart, allocate);

    wasRope.link(&jit);
    goOnWithBaseOfSubstring(jit, isLeaf, make, allocate);

    is16Bit.link(&jit);
    auto trim16 = jit.label();
    auto isTrimmedOf16Bits = trim(true);
    if (!(ends & 2))
        isTrimmedOf16Bits.append(jit.jump());
    isTrimmedOf16Bits.linkTo(isTrimmed, &jit);

    isRope.link(&jit);
    isLeftToOperation.append(jit.branchTestPtr(CCallHelpers::Zero, T14, TrustedImm32(JSRopeString::isSubstringInPointer)));
    loadBaseAndOffsetOfSubstring(jit, T11, T12, T13);
    jit.loadPtr(Address(T12, JSString::offsetOfValue()), T12);
    jit.loadPtr(Address(T12, StringImpl::dataOffset()), result);
    jit.addPtr(T13, result);
    Jump isPartOf16Bits = jit.branchTest32(CCallHelpers::Zero, Address(T12, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit()));
    jit.load32(Address(T11, JSRopeString::offsetOfLength()), T13);
    jit.jump().linkTo(trim8, &jit);
    isPartOf16Bits.link(&jit);
    jit.addPtr(T13, result);
    jit.load32(Address(T11, JSRopeString::offsetOfLength()), T13);
    jit.jump().linkTo(trim16, &jit);

    isLeftToOperationOnPart.link(&jit);
    jit.lshift64(TrustedImm32(32), T13);
    jit.or64(T13, T12);
    leaveToOperationOnString(jit, isLeaf, Entry::operationAOTTypedStringPart);

    isLeftToOperation.link(&jit);
    jit.move(TrustedImm32(ends), T12);
    leaveToOperationOnString(jit, isLeaf, Entry::operationAOTTypedStringTrim);
}
static void generateStringTrim(CCallHelpers& jit) { generateStringTrim(jit, false, false, 3); }
static void generateStringTrimStart(CCallHelpers& jit) { generateStringTrim(jit, false, false, 1); }
static void generateStringTrimEnd(CCallHelpers& jit) { generateStringTrim(jit, false, false, 2); }
static void generateStringTrimFlat8(CCallHelpers& jit) { generateStringTrim(jit, false, true, 3); }
static void generateStringTrimStartFlat8(CCallHelpers& jit) { generateStringTrim(jit, false, true, 1); }
static void generateStringTrimEndFlat8(CCallHelpers& jit) { generateStringTrim(jit, false, true, 2); }
#if CPU(ARM64)
static void generateLeafStringTrim(CCallHelpers& jit) { generateStringTrim(jit, true, false, 3); }
static void generateLeafStringTrimStart(CCallHelpers& jit) { generateStringTrim(jit, true, false, 1); }
static void generateLeafStringTrimEnd(CCallHelpers& jit) { generateStringTrim(jit, true, false, 2); }
static void generateLeafStringTrimFlat8(CCallHelpers& jit) { generateStringTrim(jit, true, true, 3); }
static void generateLeafStringTrimStartFlat8(CCallHelpers& jit) { generateStringTrim(jit, true, true, 1); }
static void generateLeafStringTrimEndFlat8(CCallHelpers& jit) { generateStringTrim(jit, true, true, 2); }
#else
static void generateLeafStringTrim(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafStringTrimStart(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafStringTrimEnd(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafStringTrimFlat8(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafStringTrimStartFlat8(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafStringTrimEndFlat8(CCallHelpers& jit) { jit.breakpoint(); }
#endif

static void toEntryWord(CCallHelpers&, GPRReg instance, GPRReg result, GPRReg scratch);

// The word of a closure of the typed compiler as an EntryWord: the address of its code, with the flags where the code that JITCode::m_entry
// goes through as well looks for them. `instance`: the one the closure is of. `closure` may be `result`.
static void loadEntryWordOfClosure(CCallHelpers& jit, GPRReg closure, GPRReg instance, GPRReg result, GPRReg scratch, ptrdiff_t offsetOfWord = Typed::Closure::offsetOfEntry())
{
    constexpr unsigned distance = ClosureWord::constructsOrdinaryObjectBit - EntryWord::constructsOrdinaryObjectBit;
    static_assert(ClosureWord::numberOfParametersShift - EntryWord::numberOfParametersShift == distance && ClosureWord::isListBit - EntryWord::isListBit == distance && ClosureWord::isNotCallableBit - EntryWord::isNotCallableBit == distance);
    static_assert(ClosureWord::isNotCallableBit == 63 && EntryWord::constructsOrdinaryObjectBit == EntryWord::numberOfAddressBits);
    ASSERT(noOverlap(instance, result, scratch) && closure != scratch);
    static_assert(ClosureWord::numberOfOffsetBits == 32);
    jit.load64(Address(closure, offsetOfWord), result);
    toEntryWord(jit, instance, result, scratch);
}

static void toEntryWord(CCallHelpers& jit, GPRReg instance, GPRReg result, GPRReg scratch)
{
#if CPU(ARM64)
    static_assert(ClosureWord::offsetUnit == 4);
    jit.loadPtr(Address(instance, Instance::offsetOfCode()), scratch);
    jit.m_assembler.add<64>(scratch, scratch, result, ARM64Assembler::UXTW, 2);
    jit.urshift64(TrustedImm32(ClosureWord::constructsOrdinaryObjectBit), result);
    jit.m_assembler.orr<64>(result, scratch, result, ARM64Assembler::LSL, EntryWord::constructsOrdinaryObjectBit);
#else
    jit.zeroExtend32ToWord(result, scratch);
    jit.urshift64(TrustedImm32(ClosureWord::constructsOrdinaryObjectBit), result);
    jit.lshift64(TrustedImm32(EntryWord::constructsOrdinaryObjectBit), result);
    if constexpr (ClosureWord::offsetUnit == 4)
        jit.lshift64(TrustedImm32(2), scratch);
    else
        static_assert(ClosureWord::offsetUnit == 1);
    jit.add64(scratch, result);
    jit.add64(Address(instance, Instance::offsetOfCode()), result);
#endif
}

// The address of the code, of a ClosureWord.
static void toAddressOfCode(CCallHelpers& jit, GPRReg word, GPRReg scratch)
{
    ASSERT(noOverlap(word, scratch, instanceGPR));
    static_assert(ClosureWord::numberOfOffsetBits == 32);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfCode()), scratch);
#if CPU(ARM64)
    static_assert(ClosureWord::offsetUnit == 4);
    jit.m_assembler.add<64>(word, scratch, word, ARM64Assembler::UXTW, 2);
#else
    static_assert(ClosureWord::offsetUnit == 4 || ClosureWord::offsetUnit == 1);
    jit.zeroExtend32ToWord(word, word);
    jit.getEffectiveAddress(CCallHelpers::BaseIndex(scratch, word, ClosureWord::offsetUnit == 4 ? CCallHelpers::TimesFour : CCallHelpers::TimesOne), word);
#endif
}

// The cell in T9 has not the structure a closure is allocated with. Its word if it is a closure of this instance all the same: an
// async function, a generator function, a function that was given a property (`F.prototype = ..`, `F.create = ..`). Zero: it is none.
static void loadClosureWordFromButterfly(CCallHelpers&, GPRReg storage, GPRReg instance, GPRReg result, GPRReg scratch);

static void generateTypedClosureEntry(CCallHelpers& jit, bool isLeaf)
{
    constexpr GPRReg result = GPRInfo::returnValueGPR;
    static_assert(noOverlap(T9, T11, T12, result));
    CCallHelpers::JumpList isNoClosure;
    jit.load8(Address(T9, JSCell::typeInfoTypeOffset()), T12);
    Jump isNotJSFunction = jit.branch32(CCallHelpers::NotEqual, T12, TrustedImm32(JSFunctionType));
    jit.load32(Address(T9, JSCell::structureIDOffset()), T11);
    structureWithID(jit, T11);
    isNoClosure.append(jit.branchPtr(CCallHelpers::NotEqual, Address(T11, Structure::offsetOfAOTInstance()), instanceGPR));
    jit.load32(Address(instanceGPR, Instance::offsetOfTypedClosureStructureID(0)), T12);
    structureWithID(jit, T12);
    jit.loadPtr(Address(T12, Structure::classInfoOffset()), T12);
    isNoClosure.append(jit.branchPtr(CCallHelpers::NotEqual, Address(T11, Structure::classInfoOffset()), T12));
    jit.loadPtr(Address(T9, Typed::Closure::offsetOfEntry()), result);
    returnToTypedCaller(jit, isLeaf);

    isNotJSFunction.link(&jit);
    jit.sub32(TrustedImm32(SlimClosureType), T12);
    static_assert(SlimClosureWithPropertiesType == SlimClosureType + 1);
    isNoClosure.append(jit.branch32(CCallHelpers::Above, T12, TrustedImm32(1)));
    jit.load32(Address(T9, JSCell::structureIDOffset()), T11);
    structureWithID(jit, T11);
    isNoClosure.append(jit.branchPtr(CCallHelpers::NotEqual, Address(T11, Structure::offsetOfAOTInstance()), instanceGPR));
    jit.loadPtr(Address(T9, Typed::SlimClosure::offsetOfWord()), result);
    Jump isAsMade = jit.branchTest32(CCallHelpers::Zero, T12);
    loadClosureWordFromButterfly(jit, result, instanceGPR, result, T11);
    isAsMade.link(&jit);
    returnToTypedCaller(jit, isLeaf);

    isNoClosure.link(&jit);
    jit.move(TrustedImm32(0), result);
    returnToTypedCaller(jit, isLeaf);
}
static void generateTypedClosureEntry(CCallHelpers& jit) { generateTypedClosureEntry(jit, false); }
#if CPU(ARM64)
static void generateLeafTypedClosureEntry(CCallHelpers& jit) { generateTypedClosureEntry(jit, true); }
#else
static void generateLeafTypedClosureEntry(CCallHelpers& jit) { jit.breakpoint(); }
#endif

static void generateIsArrayAsMade(CCallHelpers& jit, bool isLeaf)
{
    constexpr GPRReg result = GPRInfo::returnValueGPR;
    static_assert(noOverlap(T9, result));
    jit.load8(Address(T9, JSCell::typeInfoTypeOffset()), result);
    Jump isOfAnotherType = jit.branch32(CCallHelpers::NotEqual, result, TrustedImm32(ArrayType));
    jit.load32(Address(T9, JSCell::structureIDOffset()), result);
    structureWithID(jit, result);
    jit.loadPtr(Address(result, Structure::realmOffset()), result);
    Jump isOfAnotherRealm = jit.branchPtr(CCallHelpers::NotEqual, Address(instanceGPR, Instance::offsetOfGlobalObject()), result);
    jit.move(TrustedImm32(1), result);
    returnToTypedCaller(jit, isLeaf);

    isOfAnotherType.link(&jit);
    isOfAnotherRealm.link(&jit);
    jit.move(TrustedImm32(0), result);
    returnToTypedCaller(jit, isLeaf);
}
static void generateIsArrayAsMade(CCallHelpers& jit) { generateIsArrayAsMade(jit, false); }
#if CPU(ARM64)
static void generateLeafIsArrayAsMade(CCallHelpers& jit) { generateIsArrayAsMade(jit, true); }
#else
static void generateLeafIsArrayAsMade(CCallHelpers& jit) { jit.breakpoint(); }
#endif

// T9: a ClosureRequest. From the third argument register on: what the closure captures, by slot. Yields the closure, made as Lowering::lowerNewClosure() makes one in place. On ARM64 every register is kept but that and the temporaries.
static void generateNewTypedClosure(CCallHelpers& jit, bool isLeaf, unsigned numberOfCaptures, bool isMadeOnce = false, bool variableIsCell32 = false)
{
    constexpr GPRReg cell = GPRInfo::returnValueGPR;
#if CPU(ARM64)
    static_assert(ClosureRequest::maxNumberOfCaptures == 4 && noOverlap(cell, T9, T11, T12, T13, A1, A2, A3, A4, A5));
#else
    static_assert(ClosureRequest::maxNumberOfCaptures == 2 && noOverlap(cell, T9, T11, T12, T13, A1, A2, A3));
    static_assert(noOverlap(cell, T9, T12, T13, A1, A2, A3, A4, A5));
#endif
    static_assert(ClosureRequest::maxNumberOfCapturesOfSlimClosure == 4 && sizeof(Instance::TypedAllocation) == 32);
    if (numberOfCaptures > ClosureRequest::maxNumberOfCapturesOfSlimClosure) {
        jit.breakpoint();
        return;
    }
    CCallHelpers::JumpList slow;
    Jump isSlim = jit.branchTest32(CCallHelpers::NonZero, T9, TrustedImm32(static_cast<int32_t>(1u << ClosureRequest::isSlimBit)));
    if (numberOfCaptures > ClosureRequest::maxNumberOfCaptures)
        jit.breakpoint();
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfTypedAllocationRows()), T11);
    jit.move(T9, T12);
    jit.urshift64(TrustedImm32(ClosureRequest::allocationShift), T12);
    jit.load16(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesTwo), T12);
    slow.append(jit.branchTest32(CCallHelpers::Zero, T12));
#if CPU(ARM64)
    jit.subLeftShift64(T11, T12, TrustedImm32(5), T11);
#else
    jit.lshift64(TrustedImm32(5), T12);
    jit.subPtr(T12, T11);
#endif
    jit.loadPtr(Address(T11, OBJECT_OFFSETOF(Instance::TypedAllocation, allocator)), T12);
    jit.emitAllocateWithNonNullAllocator(cell, JITAllocator::variable(), T12, T13, slow, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
    jit.load64(Address(T11, OBJECT_OFFSETOF(Instance::TypedAllocation, header)), T13);
    jit.store64(T13, Address(cell, 0));
    jit.store64(CCallHelpers::TrustedImm64(0), Address(cell, JSObject::butterflyOffset()));
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfGlobalObject()), T13);
    jit.storePtr(T13, Address(cell, JSCallee::offsetOfScopeChain()));
    jit.urshift32(T9, TrustedImm32(ClosureRequest::executableShift), T12);
    jit.loadPtr(CCallHelpers::BaseIndex(instanceGPR, T12, CCallHelpers::TimesEight, Instance::offsetOfTypedExecutable(0)), T13);
    jit.storePtr(T13, Address(cell, JSFunction::offsetOfExecutableOrRareData()));
    for (unsigned slot = 0; slot < numberOfCaptures; ++slot)
        jit.store64(GPRInfo::toArgumentRegister(2 + slot), Address(cell, Typed::Closure::offsetOfCaptures() + slot * sizeof(uint64_t)));
    // Last: it says how many of the words above the collector follows.
    jit.and32(TrustedImm32(ClosureWord::functionMask), T9, T12);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfTypedFunctions()), T13);
    jit.load64(CCallHelpers::BaseIndex(T13, T12, CCallHelpers::TimesEight), T13);
    jit.store64(T13, Address(cell, Typed::Closure::offsetOfEntry()));
#if CPU(ARM64)
    Jump isNotFenced = jit.branchTest8(CCallHelpers::Zero, Address(instanceGPR, Instance::offsetOfMutatorShouldBeFenced()));
    jit.storeFence();
    isNotFenced.link(&jit);
#endif
    Jump isMade;
    if (isMadeOnce)
        isMade = jit.jump();
    else
        returnToTypedCaller(jit, isLeaf);

    isSlim.link(&jit);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfTypedSlimClosureAllocator(numberOfCaptures)), T12);
    slow.append(jit.branchTestPtr(CCallHelpers::Zero, T12));
    jit.emitAllocateWithNonNullAllocator(cell, JITAllocator::variable(), T12, T13, slow, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
    jit.urshift32(T9, TrustedImm32(ClosureRequest::executableShift), T12);
    jit.load64(CCallHelpers::BaseIndex(instanceGPR, T12, CCallHelpers::TimesEight, Instance::offsetOfTypedSlimClosureHeader(0) - static_cast<ptrdiff_t>(sizeof(uint64_t) << (ClosureRequest::isSlimBit - ClosureRequest::executableShift))), T13);
    jit.store64(T13, Address(cell, 0));
    for (unsigned slot = 0; slot < numberOfCaptures; ++slot)
        jit.store64(GPRInfo::toArgumentRegister(2 + slot), Address(cell, Typed::SlimClosure::offsetOfCaptures() + slot * sizeof(uint64_t)));
    jit.and32(TrustedImm32(ClosureWord::functionMask), T9, T12);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfTypedFunctions()), T13);
    jit.load64(CCallHelpers::BaseIndex(T13, T12, CCallHelpers::TimesEight), T13);
    jit.store64(T13, Address(cell, Typed::SlimClosure::offsetOfWord()));
#if CPU(ARM64)
    Jump slimIsNotFenced = jit.branchTest8(CCallHelpers::Zero, Address(instanceGPR, Instance::offsetOfMutatorShouldBeFenced()));
    jit.storeFence();
    slimIsNotFenced.link(&jit);
#endif
    if (isMadeOnce) {
        RELEASE_ASSERT(numberOfCaptures == 1);
        isMade.link(&jit);
        if (variableIsCell32) {
            jit.urshift64(cell, TrustedImm32(Typed::cell32Shift), T12);
            jit.store32(T12, Address(A3));
        } else
            jit.store64(cell, Address(A3));
        jit.load8(Address(A2, JSCell::cellStateOffset()), T12);
        Jump isWithinThreshold = jit.branch32(CCallHelpers::BelowOrEqual, T12, Address(instanceGPR, Instance::offsetOfBarrierThreshold()));
        returnToTypedCaller(jit, isLeaf);
        isWithinThreshold.link(&jit);
        Jump mutatorIsNotFenced = jit.branchTest8(CCallHelpers::Zero, Address(instanceGPR, Instance::offsetOfMutatorShouldBeFenced()));
        jit.memoryFence();
        jit.load8(Address(A2, JSCell::cellStateOffset()), T12);
        Jump isBlack = jit.branch32(CCallHelpers::BelowOrEqual, T12, TrustedImm32(blackThreshold));
        returnToTypedCaller(jit, isLeaf);
        mutatorIsNotFenced.link(&jit);
        isBlack.link(&jit);
#if CPU(ARM64)
        jit.move(TrustedImm32(static_cast<unsigned>(Entry::operationAOTTypedBarrierSlow) * sizeof(void*)), T9);
        generateColdOperation(jit, isLeaf, false, false, true, false, true, std::nullopt, A2);
#else
        preservingRegistersOfCaller(jit, false, [&](unsigned) {
            jit.move(A2, A1);
            jit.move(instanceGPR, A0);
            jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T11);
            jit.loadPtr(Address(T11, static_cast<unsigned>(Entry::operationAOTTypedBarrierSlow) * sizeof(void*)), T11);
            jit.call(T11, OperationPtrTag);
        });
        jit.ret();
#endif
    } else
        returnToTypedCaller(jit, isLeaf);

    slow.link(&jit);
    uint64_t count = static_cast<uint64_t>(numberOfCaptures) << ClosureRequest::countShift | static_cast<uint64_t>(isMadeOnce) << ClosureRequest::isMadeOnceBit | static_cast<uint64_t>(variableIsCell32) << ClosureRequest::variableIsCell32Bit;
#if CPU(ARM64)
    jit.move(T9, T12);
    if (count)
        jit.or64(CCallHelpers::TrustedImm64(static_cast<int64_t>(count)), T12);
    jit.move(TrustedImm32(static_cast<unsigned>(Entry::operationAOTTypedNewClosureSlow) * sizeof(void*)), T9);
    generateColdOperation(jit, isLeaf, true, false, true, false, true, std::nullopt, T12);
#else
    preservingRegistersOfCaller(jit, true, [&](unsigned) {
        jit.move(T9, A1);
        if (count) {
            jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(count)), cell);
            jit.or64(cell, A1);
        }
        jit.move(instanceGPR, A0);
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), cell);
        jit.loadPtr(Address(cell, static_cast<unsigned>(Entry::operationAOTTypedNewClosureSlow) * sizeof(void*)), cell);
        jit.call(cell, OperationPtrTag);
    });
    jit.ret();
#endif
}
static void generateInitializeTypedClosure(CCallHelpers& jit, unsigned numberOfCaptures)
{
    constexpr GPRReg cell = GPRInfo::returnValueGPR;
    static_assert(noOverlap(cell, T9, T12, T13, A2, A3, A4, A5));
    if (numberOfCaptures > ClosureRequest::maxNumberOfCapturesOfSlimClosure) {
        jit.breakpoint();
        return;
    }
    Jump isSlim = jit.branchTest32(CCallHelpers::NonZero, T9, TrustedImm32(static_cast<int32_t>(1u << ClosureRequest::isSlimBit)));
    jit.load32(Address(instanceGPR, Instance::offsetOfTypedClosureStructureID(0)), T13);
    jit.store32(T13, Address(cell, JSCell::structureIDOffset()));
    jit.move(TrustedImm32(TypeInfoBlob(NonArray, TypeInfo(JSFunctionType, Typed::Closure::StructureFlags)).blob()), T13);
    jit.store32(T13, Address(cell, JSCell::indexingTypeAndMiscOffset()));
    jit.store64(CCallHelpers::TrustedImm64(0), Address(cell, JSObject::butterflyOffset()));
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfGlobalObject()), T13);
    jit.storePtr(T13, Address(cell, JSCallee::offsetOfScopeChain()));
    jit.urshift32(T9, TrustedImm32(ClosureRequest::executableShift), T12);
    jit.loadPtr(CCallHelpers::BaseIndex(instanceGPR, T12, CCallHelpers::TimesEight, Instance::offsetOfTypedExecutable(0)), T13);
    jit.storePtr(T13, Address(cell, JSFunction::offsetOfExecutableOrRareData()));
    for (unsigned slot = 0; slot < numberOfCaptures; ++slot)
        jit.store64(GPRInfo::toArgumentRegister(2 + slot), Address(cell, Typed::Closure::offsetOfCaptures() + slot * sizeof(uint64_t)));
    jit.and32(TrustedImm32(ClosureWord::functionMask), T9, T12);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfTypedFunctions()), T13);
    jit.load64(CCallHelpers::BaseIndex(T13, T12, CCallHelpers::TimesEight), T13);
    jit.store64(T13, Address(cell, Typed::Closure::offsetOfEntry()));
    jit.ret();

    isSlim.link(&jit);
    jit.urshift32(T9, TrustedImm32(ClosureRequest::executableShift), T12);
    jit.load64(CCallHelpers::BaseIndex(instanceGPR, T12, CCallHelpers::TimesEight, Instance::offsetOfTypedSlimClosureHeader(0) - static_cast<ptrdiff_t>(sizeof(uint64_t) << (ClosureRequest::isSlimBit - ClosureRequest::executableShift))), T13);
    jit.store64(T13, Address(cell, 0));
    for (unsigned slot = 0; slot < numberOfCaptures; ++slot)
        jit.store64(GPRInfo::toArgumentRegister(2 + slot), Address(cell, Typed::SlimClosure::offsetOfCaptures() + slot * sizeof(uint64_t)));
    jit.and32(TrustedImm32(ClosureWord::functionMask), T9, T12);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfTypedFunctions()), T13);
    jit.load64(CCallHelpers::BaseIndex(T13, T12, CCallHelpers::TimesEight), T13);
    jit.store64(T13, Address(cell, Typed::SlimClosure::offsetOfWord()));
    jit.ret();
}
static void generateInitializeTypedClosure0(CCallHelpers& jit) { generateInitializeTypedClosure(jit, 0); }
static void generateLeafInitializeTypedClosure0(CCallHelpers& jit) { jit.breakpoint(); }
static void generateInitializeTypedClosure1(CCallHelpers& jit) { generateInitializeTypedClosure(jit, 1); }
static void generateLeafInitializeTypedClosure1(CCallHelpers& jit) { jit.breakpoint(); }
static void generateInitializeTypedClosure2(CCallHelpers& jit) { generateInitializeTypedClosure(jit, 2); }
static void generateLeafInitializeTypedClosure2(CCallHelpers& jit) { jit.breakpoint(); }
static void generateInitializeTypedClosure3(CCallHelpers& jit) { generateInitializeTypedClosure(jit, 3); }
static void generateLeafInitializeTypedClosure3(CCallHelpers& jit) { jit.breakpoint(); }
static void generateInitializeTypedClosure4(CCallHelpers& jit) { generateInitializeTypedClosure(jit, 4); }
static void generateLeafInitializeTypedClosure4(CCallHelpers& jit) { jit.breakpoint(); }

static void generateMaterializeEnvironment(CCallHelpers& jit)
{
    constexpr GPRReg cell = GPRInfo::returnValueGPR;
    constexpr GPRReg source = T14;
    static_assert(noOverlap(cell, source, T9, T11, T12, T13, A1, A2));
    static_assert(sizeof(Instance::TypedAllocation) == 32);
    jit.load8(Address(cell, JSCell::cellStateOffset()), T12);
    Jump isOnHeap = jit.branch32(CCallHelpers::NotEqual, T12, TrustedImm32(UINT8_MAX));
    jit.move(cell, source);
    CCallHelpers::JumpList slow;
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfTypedAllocationRows()), T11);
    jit.zeroExtend32ToWord(T9, T12);
    jit.load16(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesTwo), T12);
    slow.append(jit.branchTest32(CCallHelpers::Zero, T12));
#if CPU(ARM64)
    jit.subLeftShift64(T11, T12, TrustedImm32(5), T11);
#else
    jit.lshift64(TrustedImm32(5), T12);
    jit.subPtr(T12, T11);
#endif
    jit.loadPtr(Address(T11, OBJECT_OFFSETOF(Instance::TypedAllocation, allocator)), T12);
    jit.emitAllocateWithNonNullAllocator(cell, JITAllocator::variable(), T12, T13, slow, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
    jit.load64(Address(T11, OBJECT_OFFSETOF(Instance::TypedAllocation, header)), T13);
    jit.store64(T13, Address(cell, 0));
    jit.urshift64(T9, TrustedImm32(32), T12);
    jit.move(TrustedImm32(sizeof(JSCell)), T11);
    auto loop = jit.label();
    jit.load64(CCallHelpers::BaseIndex(source, T11, CCallHelpers::TimesOne), T13);
    jit.store64(T13, CCallHelpers::BaseIndex(cell, T11, CCallHelpers::TimesOne));
    jit.add64(TrustedImm32(sizeof(uint64_t)), T11);
    jit.branch64(CCallHelpers::Below, T11, T12).linkTo(loop, &jit);
#if CPU(ARM64)
    Jump isNotFenced = jit.branchTest8(CCallHelpers::Zero, Address(instanceGPR, Instance::offsetOfMutatorShouldBeFenced()));
    jit.storeFence();
    isNotFenced.link(&jit);
#endif
    jit.ret();

    slow.link(&jit);
#if CPU(ARM64)
    jit.zeroExtend32ToWord(T9, T12);
    jit.move(source, T13);
    jit.move(TrustedImm32(static_cast<unsigned>(Entry::operationAOTTypedMaterializeEnvironment) * sizeof(void*)), T9);
    generateColdOperation(jit, false, true, false, true, false, true, std::nullopt, T12, T13);
#else
    preservingRegistersOfCaller(jit, true, [&](unsigned) {
        jit.move(source, A2);
        jit.zeroExtend32ToWord(T9, A1);
        jit.move(instanceGPR, A0);
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T11);
        jit.loadPtr(Address(T11, static_cast<unsigned>(Entry::operationAOTTypedMaterializeEnvironment) * sizeof(void*)), T11);
        jit.call(T11, OperationPtrTag);
    });
    jit.ret();
#endif
    isOnHeap.link(&jit);
    jit.ret();
}
static void generateLeafMaterializeEnvironment(CCallHelpers& jit) { jit.breakpoint(); }
static void generateNewTypedClosure0(CCallHelpers& jit) { generateNewTypedClosure(jit, false, 0); }
static void generateNewTypedClosure1(CCallHelpers& jit) { generateNewTypedClosure(jit, false, 1); }
static void generateNewTypedClosure2(CCallHelpers& jit) { generateNewTypedClosure(jit, false, 2); }
static void generateNewTypedClosure3(CCallHelpers& jit) { generateNewTypedClosure(jit, false, 3); }
static void generateNewTypedClosure4(CCallHelpers& jit) { generateNewTypedClosure(jit, false, 4); }
static void generateNewTypedClosureOnce(CCallHelpers& jit) { generateNewTypedClosure(jit, false, 1, true); }
static void generateNewTypedClosureOnce32(CCallHelpers& jit) { generateNewTypedClosure(jit, false, 1, true, true); }
#if CPU(ARM64)
static void generateLeafNewTypedClosure0(CCallHelpers& jit) { generateNewTypedClosure(jit, true, 0); }
static void generateLeafNewTypedClosure1(CCallHelpers& jit) { generateNewTypedClosure(jit, true, 1); }
static void generateLeafNewTypedClosure2(CCallHelpers& jit) { generateNewTypedClosure(jit, true, 2); }
static void generateLeafNewTypedClosure3(CCallHelpers& jit) { generateNewTypedClosure(jit, true, 3); }
static void generateLeafNewTypedClosure4(CCallHelpers& jit) { generateNewTypedClosure(jit, true, 4); }
static void generateLeafNewTypedClosureOnce(CCallHelpers& jit) { generateNewTypedClosure(jit, true, 1, true); }
static void generateLeafNewTypedClosureOnce32(CCallHelpers& jit) { generateNewTypedClosure(jit, true, 1, true, true); }
#else
static void generateLeafNewTypedClosure0(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafNewTypedClosure1(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafNewTypedClosure2(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafNewTypedClosure3(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafNewTypedClosure4(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafNewTypedClosureOnce(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafNewTypedClosureOnce32(CCallHelpers& jit) { jit.breakpoint(); }
#endif

static void generateAllocateTypedCell(CCallHelpers& jit) { generateAllocateTyped(jit, false, TypedCellInit::Header); }
static void generateAllocateTypedObject(CCallHelpers& jit) { generateAllocateTyped(jit, false, TypedCellInit::HeaderAndWords); }
static void generateAllocateTypedObjectFromTemplate(CCallHelpers& jit) { generateAllocateTyped(jit, false, TypedCellInit::FromTemplate); }
#if CPU(ARM64)
static void generateLeafAllocateTypedCell(CCallHelpers& jit) { generateAllocateTyped(jit, true, TypedCellInit::Header); }
static void generateLeafAllocateTypedObject(CCallHelpers& jit) { generateAllocateTyped(jit, true, TypedCellInit::HeaderAndWords); }
static void generateLeafAllocateTypedObjectFromTemplate(CCallHelpers& jit) { generateAllocateTyped(jit, true, TypedCellInit::FromTemplate); }
#else
static void generateLeafAllocateTypedCell(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafAllocateTypedObject(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafAllocateTypedObjectFromTemplate(CCallHelpers& jit) { jit.breakpoint(); }
#endif

static void generateAllocateTypedExtension(CCallHelpers& jit, unsigned size)
{
#if CPU(X86_64)
    constexpr GPRReg owner = A1;
    constexpr GPRReg storage = GPRInfo::returnValueGPR;
    constexpr GPRReg allocator = T12;
    constexpr GPRReg temporary = T11;
    static_assert(noOverlap(owner, storage, allocator, temporary));
    constexpr int32_t intervalStart = LocalAllocator::offsetOfFreeList() + FreeList::offsetOfIntervalStart();
    constexpr int32_t intervalEnd = LocalAllocator::offsetOfFreeList() + FreeList::offsetOfIntervalEnd();
    constexpr int32_t nextInterval = LocalAllocator::offsetOfFreeList() + FreeList::offsetOfNextInterval();
    constexpr int32_t secret = LocalAllocator::offsetOfFreeList() + FreeList::offsetOfSecret();
    CCallHelpers::JumpList slow;
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfAuxiliarySpace()), allocator);
    jit.loadPtr(Address(allocator, CompleteSubspace::offsetOfAllocatorForSizeStep() + MarkedSpace::sizeClassToIndex(size) * sizeof(Allocator)), allocator);
    slow.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    if (Options::forceGCSlowPaths())
        slow.append(jit.jump());
    jit.loadPtr(Address(allocator, intervalStart), storage);
    Jump intervalIsUsedUp = jit.branchPtr(CCallHelpers::AboveOrEqual, storage, Address(allocator, intervalEnd));
    auto bump = jit.label();
    jit.load32(Address(allocator, LocalAllocator::offsetOfCellSize()), temporary);
    jit.add64(temporary, Address(allocator, intervalStart));
    jit.move(TrustedImm32(0), temporary);
    for (unsigned offset = sizeof(uint64_t); offset < size; offset += sizeof(uint64_t))
        jit.store64(temporary, Address(storage, offset));
    static_assert(!Typed::offsetOfExtensionSize);
    jit.store64(TrustedImm32(size), Address(storage));
    jit.urshift64(TrustedImm32(Typed::cell32Shift), storage);
    jit.store32(storage, Address(owner, Typed::offsetOfExtension));
    jit.ret();

    intervalIsUsedUp.link(&jit);
    jit.loadPtr(Address(allocator, nextInterval), storage);
    slow.append(jit.branchTestPtr(CCallHelpers::NonZero, storage, TrustedImm32(1)));
    jit.load32(Address(allocator, secret), temporary);
    jit.xor32(Address(storage, FreeCell::offsetOfScrambledBits()), temporary);
    jit.add64(temporary, Address(allocator, nextInterval));
    jit.load32(Address(allocator, secret + 4), temporary);
    jit.xor32(Address(storage, FreeCell::offsetOfScrambledBits() + 4), temporary);
    jit.storePtr(storage, Address(allocator, intervalStart));
    jit.addPtr(storage, temporary);
    jit.storePtr(temporary, Address(allocator, intervalEnd));
    jit.jump().linkTo(bump, &jit);

    slow.link(&jit);
    static_assert(noOverlap(owner, T11, A0, A2));
    jit.move(TrustedImm32(size), T11);
    static NeverDestroyed<CCallHelpers::Label> sharedSlowWay;
    if (size != Typed::leastExtensionSize) {
        jit.jump().linkTo(sharedSlowWay.get(), &jit);
        return;
    }
    sharedSlowWay.get() = jit.label();
    preservingRegistersOfCaller(jit, true, [&](unsigned) {
        jit.move(T11, A2);
        jit.move(instanceGPR, A0);
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T11);
        jit.loadPtr(Address(T11, static_cast<unsigned>(Entry::operationAOTTypedEnsureStorageOfSize) * sizeof(void*)), T11);
        jit.call(T11, OperationPtrTag);
    });
    jit.ret();
#else
    UNUSED_PARAM(size);
    jit.breakpoint();
#endif
}
static void generateAllocateTypedExtension16(CCallHelpers& jit) { generateAllocateTypedExtension(jit, 16); }
static void generateAllocateTypedExtension32(CCallHelpers& jit) { generateAllocateTypedExtension(jit, 32); }
static void generateAllocateTypedExtension48(CCallHelpers& jit) { generateAllocateTypedExtension(jit, 48); }
static void generateAllocateTypedExtension64(CCallHelpers& jit) { generateAllocateTypedExtension(jit, 64); }
static void generateAllocateTypedExtension80(CCallHelpers& jit) { generateAllocateTypedExtension(jit, 80); }
static void generateAllocateTypedExtension96(CCallHelpers& jit) { generateAllocateTypedExtension(jit, 96); }
static void generateAllocateTypedExtension112(CCallHelpers& jit) { generateAllocateTypedExtension(jit, 112); }
static void generateAllocateTypedExtension128(CCallHelpers& jit) { generateAllocateTypedExtension(jit, 128); }
static void generateAllocateTypedExtension144(CCallHelpers& jit) { generateAllocateTypedExtension(jit, 144); }
static void generateAllocateTypedExtension160(CCallHelpers& jit) { generateAllocateTypedExtension(jit, 160); }
static void generateAllocateTypedExtension176(CCallHelpers& jit) { generateAllocateTypedExtension(jit, 176); }
static void generateAllocateTypedExtension192(CCallHelpers& jit) { generateAllocateTypedExtension(jit, 192); }
static void generateAllocateTypedExtension208(CCallHelpers& jit) { generateAllocateTypedExtension(jit, 208); }
static void generateAllocateTypedExtension224(CCallHelpers& jit) { generateAllocateTypedExtension(jit, 224); }
static void generateAllocateTypedExtension240(CCallHelpers& jit) { generateAllocateTypedExtension(jit, 240); }
static void generateAllocateTypedExtension256(CCallHelpers& jit) { generateAllocateTypedExtension(jit, 256); }

template<typename SetUp>
static void callPreservingRegistersAndReturn(CCallHelpers& jit, Entry operation, bool hasResult, const SetUp& setUp)
{
    preservingRegistersOfCaller(jit, hasResult, [&](unsigned bytes) {
        loadInstance(jit, T10);
        loadReturnAddressOfCaller(jit, bytes, T11);
        setUp();
        jit.loadPtr(Address(T10, Instance::offsetOfRuntimeTable()), T10);
        jit.loadPtr(Address(T10, static_cast<unsigned>(operation) * sizeof(void*)), T10);
        jit.call(T10, OperationPtrTag);
    });
    jit.ret();
}

static CCallHelpers::Label& writeBarrierSlowCase() { static NeverDestroyed<CCallHelpers::Label> label; return label; }
static CCallHelpers::Label& toBooleanSlowCase() { static NeverDestroyed<CCallHelpers::Label> label; return label; }

static void generateWriteBarrier(CCallHelpers& jit, GPRReg owner)
{
    jit.load8(Address(owner, JSCell::cellStateOffset()), T9);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfVM()), T11);
    Jump slow = jit.branch32(CCallHelpers::BelowOrEqual, T9, Address(T11, VM::offsetOfHeapBarrierThreshold()));
    jit.ret();

    slow.link(&jit);
    Jump isNotFenced = jit.branchTest8(CCallHelpers::Zero, Address(T11, VM::offsetOfHeapMutatorShouldBeFenced()));
    jit.memoryFence();
    Jump isBlack = jit.barrierBranchWithoutFence(owner, true);
    jit.ret();

    isNotFenced.link(&jit);
    isBlack.link(&jit);
    jit.move(owner, T9);
    if (owner != R0) {
        jit.jump().linkTo(writeBarrierSlowCase(), &jit);
        return;
    }
    writeBarrierSlowCase() = jit.label();
    callPreservingRegistersAndReturn(jit, Entry::operationAOTWriteBarrier, false, [&] {
        jit.move(T9, A1);
        jit.loadPtr(Address(T10, Instance::offsetOfVM()), A0);
    });
}
static void generateWriteBarrier(CCallHelpers& jit) { generateWriteBarrier(jit, R0); }

static CCallHelpers::Label returnAfterStoring(CCallHelpers& jit, GPRReg value)
{
    Jump notCell = jit.branchIfNotCell(value);
    CCallHelpers::Label storedCell = jit.label();
    jit.load8(Address(R0, JSCell::cellStateOffset()), T11);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfVM()), T12);
    Jump barrier = jit.branch32(CCallHelpers::BelowOrEqual, T11, Address(T12, VM::offsetOfHeapBarrierThreshold()));
    notCell.link(&jit);
    jit.ret();

    barrier.link(&jit);
    Jump isNotFenced = jit.branchTest8(CCallHelpers::Zero, Address(T12, VM::offsetOfHeapMutatorShouldBeFenced()));
    jit.memoryFence();
    Jump isBlack = jit.barrierBranchWithoutFence(R0, true);
    jit.ret();

    isNotFenced.link(&jit);
    isBlack.link(&jit);
    jit.move(R0, A1);
    jit.move(T12, A0);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T9);
    jit.loadPtr(Address(T9, static_cast<unsigned>(Entry::operationAOTWriteBarrier) * sizeof(void*)), T9);
    jit.farJump(T9, OperationPtrTag);
    return storedCell;
}

static void generateToBoolean(CCallHelpers& jit, GPRReg asked)
{
    auto answer = [&](bool value) {
        jit.move(TrustedImm32(value), R0);
        jit.ret();
    };

    jit.xor64(TrustedImm32(JSValue::ValueFalse), asked, T9);
    Jump notBoolean = jit.branchTest64(CCallHelpers::NonZero, T9, TrustedImm32(~1));
    jit.move(T9, R0);
    jit.ret();

    notBoolean.link(&jit);
    Jump notInt32 = jit.branch64(CCallHelpers::Below, asked, numberTag);
    jit.test32(CCallHelpers::NonZero, asked, asked, R0);
    jit.ret();

    notInt32.link(&jit);
    Jump notNumber = jit.branchTest64(CCallHelpers::Zero, asked, numberTag);
    jit.add64(numberTag, asked, T9);
    jit.lshift64(T9, TrustedImm32(1), T9);
    Jump isZero = jit.branchTest64(CCallHelpers::Zero, T9);
    jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(0xffe0000000000000ULL)), T10);
    Jump isNaN = jit.branch64(CCallHelpers::Above, T9, T10);
    answer(true);
    isZero.link(&jit);
    isNaN.link(&jit);
    answer(false);

    notNumber.link(&jit);
    Jump isCell = jit.branchIfCell(asked);
    answer(false);

    isCell.link(&jit);
    CCallHelpers::JumpList slow;
    jit.load8(Address(asked, JSCell::typeInfoTypeOffset()), T9);
    Jump notObject = jit.branch32(CCallHelpers::Below, T9, TrustedImm32(ObjectType));
    slow.append(jit.branchTest8(CCallHelpers::NonZero, Address(asked, JSCell::typeInfoFlagsOffset()), TrustedImm32(MasqueradesAsUndefined)));
    answer(true);

    notObject.link(&jit);
    slow.append(jit.branch32(CCallHelpers::NotEqual, T9, TrustedImm32(StringType)));
    jit.loadPtr(Address(asked, JSString::offsetOfValue()), T9);
    slow.append(jit.branchIfRopeStringImpl(T9));
    jit.load32(Address(T9, StringImpl::lengthMemoryOffset()), T9);
    jit.test32(CCallHelpers::NonZero, T9, T9, R0);
    jit.ret();

    slow.link(&jit);
    jit.move(asked, T9);
    if (asked != R0) {
        jit.jump().linkTo(toBooleanSlowCase(), &jit);
        return;
    }
    toBooleanSlowCase() = jit.label();
    callPreservingRegistersAndReturn(jit, Entry::operationAOTToBoolean, true, [&] {
        jit.move(T9, A1);
        jit.move(T10, A0);
    });
}
static void generateToBoolean(CCallHelpers& jit) { generateToBoolean(jit, R0); }

enum class EqualityExits : uint8_t { None, Strict };

static void generateEqual(CCallHelpers& jit, Entry operation, EqualityExits exits = EqualityExits::None)
{
    Jump notBothInt32 = jit.branch64(CCallHelpers::Below, R0, numberTag);
    Jump notBothInt32AfterUnboxing = jit.branch64(CCallHelpers::Below, A1, numberTag);
    jit.compare32(CCallHelpers::Equal, R0, A1, R0);
    jit.ret();

    notBothInt32.link(&jit);
    notBothInt32AfterUnboxing.link(&jit);
    Jump differ = jit.branch64(CCallHelpers::NotEqual, R0, A1);
    Jump isNumber = jit.branchTest64(CCallHelpers::NonZero, R0, numberTag);
    jit.move(TrustedImm32(1), R0);
    jit.ret();

    differ.link(&jit);
    isNumber.link(&jit);
    if (exits == EqualityExits::Strict) {
        static_assert(noOverlap(R0, A1, T9, T10, T11, T12));
        CCallHelpers::JumpList isTrue;
        CCallHelpers::JumpList isFalse;
        CCallHelpers::JumpList needsContent;
        CCallHelpers::JumpList needsCharacters;
        Jump leftIsNotNumber = jit.branchTest64(CCallHelpers::Zero, R0, numberTag);
        isFalse.append(jit.branchTest64(CCallHelpers::Zero, A1, numberTag));
        auto toDouble = [&](GPRReg value, FPRReg result) {
            Jump isInt32 = jit.branch64(CCallHelpers::AboveOrEqual, value, numberTag);
            jit.add64(numberTag, value, T11);
            jit.move64ToDouble(T11, result);
            Jump done = jit.jump();
            isInt32.link(&jit);
            jit.convertInt32ToDouble(value, result);
            done.link(&jit);
        };
        toDouble(R0, FPRInfo::fpRegT0);
        toDouble(A1, FPRInfo::fpRegT1);
        isTrue.append(jit.branchDouble(CCallHelpers::DoubleEqualAndOrdered, FPRInfo::fpRegT0, FPRInfo::fpRegT1));
        isFalse.append(jit.jump());

        leftIsNotNumber.link(&jit);
        isFalse.append(jit.branchIfNotCell(R0));
        isFalse.append(jit.branchIfNotCell(A1));
        jit.load8(Address(R0, JSCell::typeInfoTypeOffset()), T9);
        jit.load8(Address(A1, JSCell::typeInfoTypeOffset()), T10);
        isFalse.append(jit.branch32(CCallHelpers::NotEqual, T9, T10));
        needsContent.append(jit.branch32(CCallHelpers::Equal, T9, TrustedImm32(HeapBigIntType)));
        isFalse.append(jit.branch32(CCallHelpers::NotEqual, T9, TrustedImm32(StringType)));
        jit.loadPtr(Address(R0, JSString::offsetOfValue()), T9);
        jit.loadPtr(Address(A1, JSString::offsetOfValue()), T10);
        jit.or64(T9, T10, T11);
        Jump oneIsRope = jit.branchTestPtr(CCallHelpers::NonZero, T11, TrustedImm32(JSString::isRopeInPointer));
        isTrue.append(jit.branchPtr(CCallHelpers::Equal, T9, T10));
        jit.load32(Address(T9, StringImpl::lengthMemoryOffset()), T11);
        jit.load32(Address(T10, StringImpl::lengthMemoryOffset()), T12);
        isFalse.append(jit.branch32(CCallHelpers::NotEqual, T11, T12));
        jit.load32(Address(T9, StringImpl::flagsOffset()), T12);
        jit.and32(Address(T10, StringImpl::flagsOffset()), T12);
        isFalse.append(jit.branchTest32(CCallHelpers::NonZero, T12, TrustedImm32(StringImpl::flagIsAtom())));
        Jump areBoth8Bit = jit.branchTest32(CCallHelpers::NonZero, T12, TrustedImm32(StringImpl::flagIs8Bit()));
        needsCharacters.append(jit.branchTest32(CCallHelpers::NonZero, Address(T9, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
        needsCharacters.append(jit.branchTest32(CCallHelpers::NonZero, Address(T10, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
        jit.lshift32(TrustedImm32(1), T11);
        areBoth8Bit.link(&jit);
        // No way leads to the operation from here on: the operands are not needed.
        constexpr GPRReg left = T9;
        constexpr GPRReg right = T10;
        constexpr GPRReg remaining = T11;
        constexpr GPRReg leftCharacters = T12;
        constexpr GPRReg rightCharacters = R0;
        jit.loadPtr(Address(left, StringImpl::dataOffset()), left);
        jit.loadPtr(Address(right, StringImpl::dataOffset()), right);
        CCallHelpers::Label hasCharacters = jit.label();
        Jump fewerThanEight = jit.branch32(CCallHelpers::Below, remaining, TrustedImm32(8));
        CCallHelpers::Label wordLoop = jit.label();
        Jump isLastWord = jit.branch32(CCallHelpers::BelowOrEqual, remaining, TrustedImm32(8));
        jit.load64(Address(left), leftCharacters);
        jit.load64(Address(right), rightCharacters);
        isFalse.append(jit.branch64(CCallHelpers::NotEqual, leftCharacters, rightCharacters));
        jit.add64(TrustedImm32(8), left);
        jit.add64(TrustedImm32(8), right);
        jit.sub32(TrustedImm32(8), remaining);
        jit.jump().linkTo(wordLoop, &jit);
        // The last eight, some of them once more.
        isLastWord.link(&jit);
        jit.add64(remaining, left);
        jit.add64(remaining, right);
        jit.load64(Address(left, -8), leftCharacters);
        jit.load64(Address(right, -8), rightCharacters);
        jit.compare64(CCallHelpers::Equal, leftCharacters, rightCharacters, R0);
        jit.ret();
        fewerThanEight.link(&jit);
        CCallHelpers::Label byteLoop = jit.label();
        isTrue.append(jit.branchTest32(CCallHelpers::Zero, remaining));
        jit.load8(Address(left), leftCharacters);
        jit.load8(Address(right), rightCharacters);
        isFalse.append(jit.branch32(CCallHelpers::NotEqual, leftCharacters, rightCharacters));
        jit.add64(TrustedImm32(1), left);
        jit.add64(TrustedImm32(1), right);
        jit.sub32(TrustedImm32(1), remaining);
        jit.jump().linkTo(byteLoop, &jit);

        oneIsRope.link(&jit);
        auto loadLength = [&](GPRReg string, GPRReg value, GPRReg length) {
            Jump isRope = jit.branchIfRopeStringImpl(value);
            jit.load32(Address(value, StringImpl::lengthMemoryOffset()), length);
            Jump isLoaded = jit.jump();
            isRope.link(&jit);
            jit.load32(Address(string, JSRopeString::offsetOfLength()), length);
            isLoaded.link(&jit);
        };
        loadLength(R0, T9, T11);
        loadLength(A1, T10, T12);
        isFalse.append(jit.branch32(CCallHelpers::NotEqual, T11, T12));
        loadCharacters8(jit, R0, T9, T12, needsCharacters);
        loadCharacters8(jit, A1, T10, T12, needsCharacters);
        jit.jump().linkTo(hasCharacters, &jit);
        needsCharacters.link(&jit);
        loadInstance(jit, T11);
        jit.move(A1, A2);
        jit.move(R0, A1);
        jit.move(T11, A0);
        jit.loadPtr(Address(T11, Instance::offsetOfRuntimeTable()), T11);
        jit.loadPtr(Address(T11, static_cast<unsigned>(Entry::operationAOTTypedStringsAreEqual) * sizeof(void*)), T11);
        callAndCheckException(jit, T11, Returns::Value, GPRInfo::returnValueGPR, false, false);

        isFalse.link(&jit);
        jit.move(TrustedImm32(0), R0);
        jit.ret();
        isTrue.link(&jit);
        jit.move(TrustedImm32(1), R0);
        jit.ret();
        needsContent.link(&jit);
    }
    loadInstance(jit, T11);
    jit.move(A1, A2);
    jit.move(R0, A1);
    jit.move(T11, A0);
    jit.loadPtr(Address(T11, Instance::offsetOfRuntimeTable()), T11);
    jit.loadPtr(Address(T11, static_cast<unsigned>(operation) * sizeof(void*)), T11);
    callAndCheckException(jit, T11, Returns::Value);
}

static void generateStrictEqual(CCallHelpers& jit) { generateEqual(jit, Entry::operationAOTCompareStrictEq, EqualityExits::Strict); }

static void generateInstanceOf(CCallHelpers& jit)
{
    CCallHelpers::JumpList slowPath;
    slowPath.append(jit.branchIfNotCell(A1));
    slowPath.append(jit.branchIfNotObject(A1));
    jit.move(R0, T11);

    CCallHelpers::Label loop = jit.label();
    slowPath.append(jit.branchTest8(CCallHelpers::NonZero, Address(T11, JSObject::typeInfoFlagsOffset()), TrustedImm32(OverridesGetPrototype)));
    jit.load32(Address(T11, JSCell::structureIDOffset()), T12);
    structureWithID(jit, T12);
    jit.load64(Address(T12, Structure::prototypeOffset()), T12);
    Jump hasMonoProto = jit.branchTest64(CCallHelpers::NonZero, T12);
    jit.load64(Address(T11, offsetRelativeToBase(knownPolyProtoOffset)), T12);
    hasMonoProto.link(&jit);
    Jump isInstance = jit.branch64(CCallHelpers::Equal, T12, A1);
    jit.move(T12, T11);
    jit.branchIfCell(T11).linkTo(loop, &jit);
    jit.move(TrustedImm32(0), R0);
    jit.ret();

    isInstance.link(&jit);
    jit.move(TrustedImm32(1), R0);
    jit.ret();

    slowPath.link(&jit);
    loadInstance(jit, T11);
    jit.move(A1, A2);
    jit.move(R0, A1);
    jit.move(T11, A0);
    jit.loadPtr(Address(T11, Instance::offsetOfRuntimeTable()), T11);
    jit.loadPtr(Address(T11, static_cast<unsigned>(Entry::operationAOTDefaultHasInstance) * sizeof(void*)), T11);
    callAndCheckException(jit, T11, Returns::Value);
}

static void generateLooseEqual(CCallHelpers& jit) { generateEqual(jit, Entry::operationAOTCompareEq); }

static void callBinaryOperation(CCallHelpers& jit, Entry operation)
{
    loadInstance(jit, T11);
    jit.move(A1, A2);
    jit.move(R0, A1);
    jit.move(T11, A0);
    jit.loadPtr(Address(T11, Instance::offsetOfRuntimeTable()), T11);
    jit.loadPtr(Address(T11, static_cast<unsigned>(operation) * sizeof(void*)), T11);
    callAndCheckException(jit, T11, Returns::Value);
}

static void fenceAfterAllocation(CCallHelpers&, GPRReg scratch);

static void makeRopeOfTwoStrings(CCallHelpers& jit, CCallHelpers::JumpList& slow)
{
    constexpr GPRReg first = R0;
    constexpr GPRReg second = A1;
    constexpr GPRReg flags = T9;
    constexpr GPRReg length = T10;
    constexpr GPRReg scratch = T11;
    constexpr GPRReg rope = T12;
    constexpr GPRReg allocator = T13;
    constexpr GPRReg lengthOfSecond = rope;
    static_assert(noOverlap(first, second, flags, length, scratch, rope, allocator));
    static_assert(StringImpl::flagIs8Bit() == JSRopeString::is8BitInPointer);
    static_assert(JSString::MaxLength == std::numeric_limits<int32_t>::max());
    static_assert(JSRopeString::offsetOfFiber0() == sizeof(EncodedJSValue) && JSRopeString::offsetOfLength() == 2 * sizeof(EncodedJSValue));
    auto loadFlagsAndLength = [&](GPRReg string, GPRReg flagsResult, GPRReg lengthResult) {
        jit.loadPtr(Address(string, JSString::offsetOfValue()), flagsResult);
        Jump isRope = jit.branchIfRopeStringImpl(flagsResult);
        jit.load32(Address(flagsResult, StringImpl::lengthMemoryOffset()), lengthResult);
        jit.load32(Address(flagsResult, StringImpl::flagsOffset()), flagsResult);
        Jump isLoaded = jit.jump();
        isRope.link(&jit);
        jit.load32(Address(string, JSRopeString::offsetOfLength()), lengthResult);
        isLoaded.link(&jit);
    };

    slow.append(jit.branchIfNotCell(first));
    slow.append(jit.branchIfNotCell(second));
    slow.append(jit.branchIfNotString(first));
    slow.append(jit.branchIfNotString(second));
    loadFlagsAndLength(first, flags, length);
    Jump firstIsEmpty = jit.branchTest32(CCallHelpers::Zero, length);
    loadFlagsAndLength(second, scratch, lengthOfSecond);
    Jump secondIsEmpty = jit.branchTest32(CCallHelpers::Zero, lengthOfSecond);
    jit.and32(scratch, flags);
    slow.append(jit.branchAdd32(CCallHelpers::Overflow, lengthOfSecond, length));
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfRopeStringAllocator()), allocator);
    slow.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    jit.emitAllocateWithNonNullAllocator(rope, JITAllocator::variable(), allocator, scratch, slow, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
    jit.load32(Address(instanceGPR, Instance::offsetOfStringStructureID()), scratch);
    jit.or64(CCallHelpers::TrustedImm64(static_cast<uint64_t>(TypeInfoBlob(NonArray, TypeInfo(StringType, JSString::StructureFlags)).blob()) << 32), scratch);
    jit.and32(TrustedImm32(JSRopeString::is8BitInPointer), flags);
    jit.or64(first, flags);
    jit.or64(TrustedImm32(JSString::isRopeInPointer), flags);
    jit.storePair64(scratch, flags, rope, TrustedImm32(0));
    jit.move(second, scratch);
    jit.lshift64(TrustedImm32(32), scratch);
    jit.or64(length, scratch);
    jit.move(second, allocator);
    jit.urshift64(TrustedImm32(32), allocator);
    jit.storePair64(scratch, allocator, rope, TrustedImm32(2 * sizeof(EncodedJSValue)));
    fenceAfterAllocation(jit, scratch);
    jit.move(rope, R0);
    jit.ret();

    firstIsEmpty.link(&jit);
    jit.move(second, R0);
    secondIsEmpty.link(&jit);
    jit.ret();
}

// Two or three strings, where Lowering::concatenateByStub() puts them. Yields their concatenation, as Lowering::concatenate() makes it in place. Every register is kept but the temporaries.
static void generateConcatenate(CCallHelpers& jit, bool isLeaf, bool hasThird)
{
    constexpr GPRReg result = GPRInfo::returnValueGPR;
#if CPU(ARM64)
    constexpr GPRReg first = T11;
    constexpr GPRReg second = T12;
    constexpr GPRReg third = A3;
    constexpr GPRReg flags = T9;
    constexpr GPRReg length = T14;
    constexpr GPRReg scratch = T15;
    constexpr GPRReg rope = result;
    constexpr GPRReg allocator = T13;
    static_assert(noOverlap(T10, first, second, third, flags, length, scratch, rope, allocator));
#else
    constexpr GPRReg first = result;
    constexpr GPRReg second = A1;
    constexpr GPRReg third = A2;
    constexpr GPRReg flags = T9;
    constexpr GPRReg length = T10;
    constexpr GPRReg scratch = T11;
    constexpr GPRReg rope = T12;
    constexpr GPRReg allocator = T13;
#endif
    constexpr GPRReg lengthOfNext = rope;
    static_assert(noOverlap(first, second, third, flags, length, scratch, rope, allocator));
    static_assert(StringImpl::flagIs8Bit() == JSRopeString::is8BitInPointer);
    static_assert(JSString::MaxLength == std::numeric_limits<int32_t>::max());
    static_assert(JSRopeString::offsetOfFiber0() == sizeof(EncodedJSValue) && JSRopeString::offsetOfLength() == 2 * sizeof(EncodedJSValue));
    CCallHelpers::JumpList slow;
    auto loadFlagsAndLength = [&](GPRReg string, GPRReg flagsResult, GPRReg lengthResult) {
        jit.loadPtr(Address(string, JSString::offsetOfValue()), flagsResult);
        Jump isRope = jit.branchIfRopeStringImpl(flagsResult);
        jit.load32(Address(flagsResult, StringImpl::lengthMemoryOffset()), lengthResult);
        jit.load32(Address(flagsResult, StringImpl::flagsOffset()), flagsResult);
        Jump isLoaded = jit.jump();
        isRope.link(&jit);
        jit.load32(Address(string, JSRopeString::offsetOfLength()), lengthResult);
        isLoaded.link(&jit);
    };
    Jump firstIsEmpty;
    Jump secondIsEmpty;
    Jump allAreEmpty;
    loadFlagsAndLength(first, flags, length);
    if (!hasThird)
        firstIsEmpty = jit.branchTest32(CCallHelpers::Zero, length);
    loadFlagsAndLength(second, scratch, lengthOfNext);
    if (!hasThird)
        secondIsEmpty = jit.branchTest32(CCallHelpers::Zero, lengthOfNext);
    jit.and32(scratch, flags);
    slow.append(jit.branchAdd32(CCallHelpers::Overflow, lengthOfNext, length));
    if (hasThird) {
        loadFlagsAndLength(third, scratch, lengthOfNext);
        jit.and32(scratch, flags);
        slow.append(jit.branchAdd32(CCallHelpers::Overflow, lengthOfNext, length));
        allAreEmpty = jit.branchTest32(CCallHelpers::Zero, length);
    }
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfRopeStringAllocator()), allocator);
    slow.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    jit.emitAllocateWithNonNullAllocator(rope, JITAllocator::variable(), allocator, scratch, slow, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
    jit.load64(Address(instanceGPR, Instance::offsetOfStringHeader()), scratch);
    jit.and32(TrustedImm32(JSRopeString::is8BitInPointer), flags);
    jit.or64(first, flags);
    jit.or64(TrustedImm32(JSString::isRopeInPointer), flags);
    jit.storePair64(scratch, flags, rope, TrustedImm32(0));
    jit.move(second, scratch);
    jit.lshift64(TrustedImm32(32), scratch);
    jit.or64(length, scratch);
    jit.move(second, allocator);
    jit.urshift64(TrustedImm32(32), allocator);
    if (hasThird) {
        jit.move(third, flags);
        jit.lshift64(TrustedImm32(16), flags);
        jit.or64(flags, allocator);
    }
    jit.storePair64(scratch, allocator, rope, TrustedImm32(2 * sizeof(EncodedJSValue)));
    fenceAfterAllocation(jit, scratch);
    jit.move(rope, result);
    returnToTypedCaller(jit, isLeaf);

    if (hasThird) {
        allAreEmpty.link(&jit);
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfEmptyString()), result);
        returnToTypedCaller(jit, isLeaf);
    } else {
        firstIsEmpty.link(&jit);
        jit.move(second, result);
        returnToTypedCaller(jit, isLeaf);
        secondIsEmpty.link(&jit);
        jit.move(first, result);
        returnToTypedCaller(jit, isLeaf);
    }

    slow.link(&jit);
    Entry operation = hasThird ? Entry::operationAOTTypedConcatThree : Entry::operationAOTTypedConcat;
#if CPU(ARM64)
    jit.move(TrustedImm32(static_cast<unsigned>(operation) * sizeof(void*)), T9);
    generateColdOperation(jit, isLeaf, true, true, true, false, true, std::nullopt, first, second);
#else
    generateColdOperationPlacingArguments(jit, operation, 0, [&] {
        static_assert(second == A1 && third == A2 && noOverlap(first, A1, A2, A3));
        if (hasThird)
            jit.move(third, A3);
        jit.move(second, A2);
        jit.move(first, A1);
    });
#endif
}
static void generateConcatenateTwo(CCallHelpers& jit) { generateConcatenate(jit, false, false); }
static void generateConcatenateThree(CCallHelpers& jit) { generateConcatenate(jit, false, true); }
#if CPU(ARM64)
static void generateLeafConcatenateTwo(CCallHelpers& jit) { generateConcatenate(jit, true, false); }
static void generateLeafConcatenateThree(CCallHelpers& jit) { generateConcatenate(jit, true, true); }
#else
static void generateLeafConcatenateTwo(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafConcatenateThree(CCallHelpers& jit) { jit.breakpoint(); }
#endif

static void generateStringOfInt32(CCallHelpers& jit, bool isLeaf)
{
    constexpr GPRReg result = GPRInfo::returnValueGPR;
#if CPU(ARM64)
    constexpr GPRReg value = T11;
    constexpr GPRReg first = T12;
    constexpr GPRReg second = T13;
    constexpr GPRReg high = T14;
#else
    constexpr GPRReg value = A1;
    constexpr GPRReg first = X86Registers::eax;
    constexpr GPRReg second = T10;
    constexpr GPRReg high = X86Registers::edx;
    static_assert(high == GPRInfo::returnValueGPR2);
#endif
    static_assert(noOverlap(value, first, second, high, T9));
    static_assert(sizeof(NumericStrings::StringWithJSString) == 2 * sizeof(void*) && sizeof(NumericStrings::CacheEntryWithJSString<int>) == 3 * sizeof(void*));
    CCallHelpers::JumpList slow;
    Jump isSmall = jit.branch32(CCallHelpers::Below, value, TrustedImm32(NumericStrings::cacheSize));
    jit.zeroExtend32ToWord(value, first);
    jit.load64(Address(instanceGPR, Instance::offsetOfHashSecrets() + sizeof(uint64_t)), second);
    jit.xor64(first, second);
    jit.load64(Address(instanceGPR, Instance::offsetOfHashSecrets()), high);
    jit.xor64(high, first);
#if CPU(ARM64)
    jit.uMulHigh64(first, second, high);
    jit.mul64(second, first);
#else
    jit.x86UMulHigh64(second, first, high);
#endif
    jit.xor32(high, first);
    jit.and32(TrustedImm32(NumericStrings::cacheSize - 1), first);
    jit.getEffectiveAddress(CCallHelpers::BaseIndex(first, first, CCallHelpers::TimesTwo), first);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfIntStrings()), second);
    jit.getEffectiveAddress(CCallHelpers::BaseIndex(second, first, CCallHelpers::TimesEight), second);
    slow.append(jit.branch32(CCallHelpers::NotEqual, Address(second, NumericStrings::CacheEntryWithJSString<int>::offsetOfKey()), value));
    jit.loadPtr(Address(second, NumericStrings::CacheEntryWithJSString<int>::offsetOfJSString()), result);
    slow.append(jit.branchTestPtr(CCallHelpers::Zero, result));
    returnToTypedCaller(jit, isLeaf);

    isSmall.link(&jit);
    jit.zeroExtend32ToWord(value, first);
    jit.lshift64(TrustedImm32(1), first);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfSmallIntStrings()), second);
    jit.loadPtr(CCallHelpers::BaseIndex(second, first, CCallHelpers::TimesEight, NumericStrings::StringWithJSString::offsetOfJSString()), result);
    slow.append(jit.branchTestPtr(CCallHelpers::Zero, result));
    returnToTypedCaller(jit, isLeaf);

    slow.link(&jit);
    jit.move(TrustedImm32(static_cast<unsigned>(Entry::operationAOTTypedIntToString) * sizeof(void*)), T9);
#if CPU(ARM64)
    generateColdOperation(jit, isLeaf, true, false, true, false, true, std::nullopt, value);
#else
    generateColdOperation(jit, false, true, false);
#endif
}
static void generateStringOfInt32(CCallHelpers& jit) { generateStringOfInt32(jit, false); }
#if CPU(ARM64)
static void generateLeafStringOfInt32(CCallHelpers& jit) { generateStringOfInt32(jit, true); }
#else
static void generateLeafStringOfInt32(CCallHelpers& jit) { jit.breakpoint(); }
#endif

enum class Binary : uint8_t { Add, Sub, Mul, Div, BitAnd, BitOr, BitXor, LShift, RShift, URShift, Mod, Less, LessEq, Greater, GreaterEq };

static void branchIfNoExactInt32(CCallHelpers& jit, FPRReg number, GPRReg integer, CCallHelpers::JumpList& isNone, FPRReg scratch)
{
#if CPU(ARM64)
    if (CCallHelpers::supportsDoubleToInt32ConversionUsingJavaScriptSemantics()) {
        jit.convertDoubleToInt32UsingJavaScriptSemantics(number, integer);
        isNone.append(jit.branchOnFlags(CCallHelpers::NotEqual));
        return;
    }
#endif
    jit.branchConvertDoubleToInt32(number, integer, isNone, scratch);
}

static void generateBinary(CCallHelpers& jit, Binary kind, Entry operation, bool isBehindInt32Test = false)
{
    RELEASE_ASSERT(!isBehindInt32Test || (kind != Binary::Div && kind != Binary::Mod));
    bool isComparison = kind >= Binary::Less;
    bool hasDoubleCase = isComparison || kind <= Binary::Div;
    bool isOnBits = kind >= Binary::BitAnd && kind <= Binary::URShift;
    constexpr FPRReg left = FPRInfo::fpRegT0;
    constexpr FPRReg right = FPRInfo::fpRegT1;
    CCallHelpers::JumpList notBothInt32;
    CCallHelpers::JumpList slow;

    if (!isBehindInt32Test && kind != Binary::Div) {
        notBothInt32.append(jit.branch64(CCallHelpers::Below, R0, numberTag));
        notBothInt32.append(jit.branch64(CCallHelpers::Below, A1, numberTag));
    }
    auto boxInt32AndReturn = [&] {
        jit.add64(numberTag, T11, R0);
        jit.ret();
    };
    auto compareAndReturn = [&](CCallHelpers::RelationalCondition condition) {
        jit.compare32(condition, R0, A1, R0);
        jit.ret();
    };
    if (!isBehindInt32Test) {
        switch (kind) {
        case Binary::Add:
            slow.append(jit.branchAdd32(CCallHelpers::Overflow, R0, A1, T11));
            boxInt32AndReturn();
            break;
        case Binary::Sub:
            slow.append(jit.branchSub32(CCallHelpers::Overflow, R0, A1, T11));
            boxInt32AndReturn();
            break;
        case Binary::Mul:
            slow.append(jit.branchMul32(CCallHelpers::Overflow, R0, A1, T11));
            slow.append(jit.branchTest32(CCallHelpers::Zero, T11));
            boxInt32AndReturn();
            break;
        case Binary::Div:
            break;
        case Binary::BitAnd:
            jit.and32(R0, A1, T11);
            boxInt32AndReturn();
            break;
        case Binary::BitOr:
            jit.or32(R0, A1, T11);
            boxInt32AndReturn();
            break;
        case Binary::BitXor:
            jit.xor32(R0, A1, T11);
            boxInt32AndReturn();
            break;
        case Binary::LShift:
            jit.lshift32(R0, A1, T11);
            boxInt32AndReturn();
            break;
        case Binary::RShift:
            jit.rshift32(R0, A1, T11);
            boxInt32AndReturn();
            break;
        case Binary::URShift:
            jit.urshift32(R0, A1, T11);
            slow.append(jit.branch32(CCallHelpers::LessThan, T11, TrustedImm32(0)));
            boxInt32AndReturn();
            break;
        case Binary::Mod:
            slow.append(jit.branch32(CCallHelpers::LessThan, R0, TrustedImm32(0)));
            slow.append(jit.branch32(CCallHelpers::LessThanOrEqual, A1, TrustedImm32(0)));
#if CPU(X86_64)
            static_assert(R0 == X86Registers::eax && A1 != X86Registers::edx);
            jit.x86ConvertToDoubleWord32();
            jit.x86Div32(A1);
            jit.move(X86Registers::edx, T11);
#else
            jit.div32(R0, A1, T11);
            jit.multiplySub32(T11, A1, R0, T11);
#endif
            boxInt32AndReturn();
            break;
        case Binary::Less:
            compareAndReturn(CCallHelpers::LessThan);
            break;
        case Binary::LessEq:
            compareAndReturn(CCallHelpers::LessThanOrEqual);
            break;
        case Binary::Greater:
            compareAndReturn(CCallHelpers::GreaterThan);
            break;
        case Binary::GreaterEq:
            compareAndReturn(CCallHelpers::GreaterThanOrEqual);
            break;
        }
    }

    notBothInt32.link(&jit);
    if (hasDoubleCase) {
        slow.append(jit.branchTest64(CCallHelpers::Zero, R0, numberTag));
        slow.append(jit.branchTest64(CCallHelpers::Zero, A1, numberTag));
        auto toDouble = [&](GPRReg value, FPRReg result) {
            Jump isInt32 = jit.branch64(CCallHelpers::AboveOrEqual, value, numberTag);
            jit.add64(numberTag, value, T11);
            jit.move64ToDouble(T11, result);
            Jump done = jit.jump();
            isInt32.link(&jit);
            jit.convertInt32ToDouble(value, result);
            done.link(&jit);
        };
        toDouble(R0, left);
        toDouble(A1, right);
        auto compareDoublesAndReturn = [&](CCallHelpers::DoubleCondition condition) {
            jit.compareDouble(condition, left, right, R0);
            jit.ret();
        };
        switch (kind) {
        case Binary::Add:
            jit.addDouble(left, right, left);
            break;
        case Binary::Sub:
            jit.subDouble(left, right, left);
            break;
        case Binary::Mul:
            jit.mulDouble(left, right, left);
            break;
        case Binary::Div:
            jit.divDouble(left, right, left);
            break;
        case Binary::Less:
            compareDoublesAndReturn(CCallHelpers::DoubleLessThanAndOrdered);
            break;
        case Binary::LessEq:
            compareDoublesAndReturn(CCallHelpers::DoubleLessThanOrEqualAndOrdered);
            break;
        case Binary::Greater:
            compareDoublesAndReturn(CCallHelpers::DoubleGreaterThanAndOrdered);
            break;
        case Binary::GreaterEq:
            compareDoublesAndReturn(CCallHelpers::DoubleGreaterThanOrEqualAndOrdered);
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
        if (!isComparison) {
            CCallHelpers::JumpList notInt32;
            branchIfNoExactInt32(jit, left, T11, notInt32, right);
            boxInt32AndReturn();
            notInt32.link(&jit);
            jit.moveDoubleTo64(left, R0);
            jit.sub64(numberTag, R0);
            jit.ret();
        }
    }

    if (isOnBits) {
        auto toInt32 = [&](GPRReg value, GPRReg result) {
            jit.move(value, result);
            Jump isInt32 = jit.branch64(CCallHelpers::AboveOrEqual, value, numberTag);
            Jump isNoNumber = jit.branchTest64(CCallHelpers::Zero, value, numberTag);
            jit.add64(numberTag, value, T11);
            jit.move64ToDouble(T11, left);
            slow.append(jit.branchTruncateDoubleToInt32(left, result));
            Jump isTruncated = jit.jump();
            isNoNumber.link(&jit);
            slow.append(jit.branchIfCell(value));
            jit.compare64(CCallHelpers::Equal, value, TrustedImm32(JSValue::ValueTrue), result);
            isTruncated.link(&jit);
            isInt32.link(&jit);
        };
        toInt32(R0, T9);
        toInt32(A1, T10);
        switch (kind) {
        case Binary::BitAnd:
            jit.and32(T9, T10, T11);
            break;
        case Binary::BitOr:
            jit.or32(T9, T10, T11);
            break;
        case Binary::BitXor:
            jit.xor32(T9, T10, T11);
            break;
        case Binary::LShift:
            jit.lshift32(T9, T10, T11);
            break;
        case Binary::RShift:
            jit.rshift32(T9, T10, T11);
            break;
        case Binary::URShift:
            jit.urshift32(T9, T10, T11);
            slow.append(jit.branch32(CCallHelpers::LessThan, T11, TrustedImm32(0)));
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
        boxInt32AndReturn();
    }

    slow.link(&jit);
    if (kind == Binary::Add) {
        CCallHelpers::JumpList notTwoStrings;
        makeRopeOfTwoStrings(jit, notTwoStrings);
        notTwoStrings.link(&jit);
    }
    if (isComparison) {
        static_assert(noOverlap(R0, A1, T9, T10, T11, T12));
        CCallHelpers::JumpList notTwoFlat8BitStrings;
        notTwoFlat8BitStrings.append(jit.branchIfNotCell(R0));
        notTwoFlat8BitStrings.append(jit.branchIfNotCell(A1));
        jit.load8(Address(R0, JSCell::typeInfoTypeOffset()), T9);
        jit.load8(Address(A1, JSCell::typeInfoTypeOffset()), T10);
        notTwoFlat8BitStrings.append(jit.branch32(CCallHelpers::NotEqual, T9, TrustedImm32(StringType)));
        notTwoFlat8BitStrings.append(jit.branch32(CCallHelpers::NotEqual, T10, TrustedImm32(StringType)));
        jit.loadPtr(Address(R0, JSString::offsetOfValue()), T9);
        jit.loadPtr(Address(A1, JSString::offsetOfValue()), T10);
        jit.or64(T9, T10, T11);
        notTwoFlat8BitStrings.append(jit.branchTestPtr(CCallHelpers::NonZero, T11, TrustedImm32(JSString::isRopeInPointer)));
        jit.load32(Address(T9, StringImpl::flagsOffset()), T11);
        jit.and32(Address(T10, StringImpl::flagsOffset()), T11);
        notTwoFlat8BitStrings.append(jit.branchTest32(CCallHelpers::Zero, T11, TrustedImm32(StringImpl::flagIs8Bit())));
        constexpr GPRReg leftCharacters = T9;
        constexpr GPRReg rightCharacters = T10;
        constexpr GPRReg difference = T11;
        constexpr GPRReg remaining = T12;
        constexpr GPRReg leftUnit = R0;
        constexpr GPRReg rightUnit = A1;
        jit.load32(Address(T9, StringImpl::lengthMemoryOffset()), leftUnit);
        jit.load32(Address(T10, StringImpl::lengthMemoryOffset()), remaining);
        jit.sub32(leftUnit, remaining, difference);
        Jump rightIsNoLonger = jit.branch32(CCallHelpers::GreaterThanOrEqual, difference, TrustedImm32(0));
        jit.move(leftUnit, remaining);
        rightIsNoLonger.link(&jit);
        jit.loadPtr(Address(T9, StringImpl::dataOffset()), leftCharacters);
        jit.loadPtr(Address(T10, StringImpl::dataOffset()), rightCharacters);
        CCallHelpers::Label wordLoop = jit.label();
        Jump fewerThanEight = jit.branch32(CCallHelpers::Below, remaining, TrustedImm32(8));
        jit.load64(Address(leftCharacters), leftUnit);
        jit.load64(Address(rightCharacters), rightUnit);
        Jump wordsDiffer = jit.branch64(CCallHelpers::NotEqual, leftUnit, rightUnit);
        jit.add64(TrustedImm32(8), leftCharacters);
        jit.add64(TrustedImm32(8), rightCharacters);
        jit.sub32(TrustedImm32(8), remaining);
        jit.jump().linkTo(wordLoop, &jit);
        fewerThanEight.link(&jit);
        wordsDiffer.link(&jit);
        CCallHelpers::Label byteLoop = jit.label();
        Jump oneIsPrefix = jit.branchTest32(CCallHelpers::Zero, remaining);
        jit.load8(Address(leftCharacters), leftUnit);
        jit.load8(Address(rightCharacters), rightUnit);
        Jump unitsDiffer = jit.branch32(CCallHelpers::NotEqual, leftUnit, rightUnit);
        jit.add64(TrustedImm32(1), leftCharacters);
        jit.add64(TrustedImm32(1), rightCharacters);
        jit.sub32(TrustedImm32(1), remaining);
        jit.jump().linkTo(byteLoop, &jit);
        bool isLess = kind == Binary::Less || kind == Binary::LessEq;
        unitsDiffer.link(&jit);
        jit.compare32(isLess ? CCallHelpers::Below : CCallHelpers::Above, leftUnit, rightUnit, R0);
        jit.ret();
        oneIsPrefix.link(&jit);
        CCallHelpers::RelationalCondition byLength = kind == Binary::Less ? CCallHelpers::LessThan : kind == Binary::LessEq ? CCallHelpers::LessThanOrEqual : kind == Binary::Greater ? CCallHelpers::GreaterThan : CCallHelpers::GreaterThanOrEqual;
        jit.compare32(byLength, difference, TrustedImm32(0), R0);
        jit.ret();
        notTwoFlat8BitStrings.link(&jit);
    }
    callBinaryOperation(jit, operation);
}

static void generateAdd(CCallHelpers& jit) { generateBinary(jit, Binary::Add, Entry::operationAOTValueAdd); }
static void generateMod(CCallHelpers& jit) { generateBinary(jit, Binary::Mod, Entry::operationAOTValueMod); }
static void generateSub(CCallHelpers& jit) { generateBinary(jit, Binary::Sub, Entry::operationAOTValueSub); }
static void generateMul(CCallHelpers& jit) { generateBinary(jit, Binary::Mul, Entry::operationAOTValueMul); }
static void generateDiv(CCallHelpers& jit) { generateBinary(jit, Binary::Div, Entry::operationAOTValueDiv); }
static void generateBitAnd(CCallHelpers& jit) { generateBinary(jit, Binary::BitAnd, Entry::operationAOTValueBitAnd); }
static void generateBitOr(CCallHelpers& jit) { generateBinary(jit, Binary::BitOr, Entry::operationAOTValueBitOr); }
static void generateBitXor(CCallHelpers& jit) { generateBinary(jit, Binary::BitXor, Entry::operationAOTValueBitXor); }
static void generateLShift(CCallHelpers& jit) { generateBinary(jit, Binary::LShift, Entry::operationAOTValueLShift); }
static void generateRShift(CCallHelpers& jit) { generateBinary(jit, Binary::RShift, Entry::operationAOTValueRShift); }
static void generateURShift(CCallHelpers& jit) { generateBinary(jit, Binary::URShift, Entry::operationAOTValueURShift); }
static void generateLess(CCallHelpers& jit) { generateBinary(jit, Binary::Less, Entry::operationAOTCompareLess); }
static void generateLessEq(CCallHelpers& jit) { generateBinary(jit, Binary::LessEq, Entry::operationAOTCompareLessEq); }
static void generateGreater(CCallHelpers& jit) { generateBinary(jit, Binary::Greater, Entry::operationAOTCompareGreater); }
static void generateGreaterEq(CCallHelpers& jit) { generateBinary(jit, Binary::GreaterEq, Entry::operationAOTCompareGreaterEq); }
static void generateAddBehindInt32Test(CCallHelpers& jit) { generateBinary(jit, Binary::Add, Entry::operationAOTValueAdd, true); }
static void generateSubBehindInt32Test(CCallHelpers& jit) { generateBinary(jit, Binary::Sub, Entry::operationAOTValueSub, true); }
static void generateMulBehindInt32Test(CCallHelpers& jit) { generateBinary(jit, Binary::Mul, Entry::operationAOTValueMul, true); }
static void generateBitAndBehindInt32Test(CCallHelpers& jit) { generateBinary(jit, Binary::BitAnd, Entry::operationAOTValueBitAnd, true); }
static void generateBitOrBehindInt32Test(CCallHelpers& jit) { generateBinary(jit, Binary::BitOr, Entry::operationAOTValueBitOr, true); }
static void generateBitXorBehindInt32Test(CCallHelpers& jit) { generateBinary(jit, Binary::BitXor, Entry::operationAOTValueBitXor, true); }
static void generateLShiftBehindInt32Test(CCallHelpers& jit) { generateBinary(jit, Binary::LShift, Entry::operationAOTValueLShift, true); }
static void generateRShiftBehindInt32Test(CCallHelpers& jit) { generateBinary(jit, Binary::RShift, Entry::operationAOTValueRShift, true); }
static void generateURShiftBehindInt32Test(CCallHelpers& jit) { generateBinary(jit, Binary::URShift, Entry::operationAOTValueURShift, true); }
static void generateLessBehindInt32Test(CCallHelpers& jit) { generateBinary(jit, Binary::Less, Entry::operationAOTCompareLess, true); }
static void generateLessEqBehindInt32Test(CCallHelpers& jit) { generateBinary(jit, Binary::LessEq, Entry::operationAOTCompareLessEq, true); }
static void generateGreaterBehindInt32Test(CCallHelpers& jit) { generateBinary(jit, Binary::Greater, Entry::operationAOTCompareGreater, true); }
static void generateGreaterEqBehindInt32Test(CCallHelpers& jit) { generateBinary(jit, Binary::GreaterEq, Entry::operationAOTCompareGreaterEq, true); }

static void checkTypedArrayAccess(CCallHelpers& jit, CCallHelpers::JumpList& slow)
{
    jit.load8(Address(R0, JSCell::typeInfoTypeOffset()), T13);
    jit.sub32(T13, TrustedImm32(FirstTypedArrayType), T11);
    slow.append(jit.branch32(CCallHelpers::AboveOrEqual, T11, TrustedImm32(NumberOfTypedArrayTypesExcludingDataView)));
    slow.append(jit.branchTest8(CCallHelpers::NonZero, Address(R0, JSArrayBufferView::offsetOfMode()), TrustedImm32(isResizableOrGrowableSharedMode)));
    jit.zeroExtend32ToWord(A1, T12);
    jit.loadPtr(Address(R0, JSArrayBufferView::offsetOfLength()), T11);
    slow.append(jit.branchPtr(CCallHelpers::AboveOrEqual, T12, T11));
    jit.loadPtr(Address(R0, JSArrayBufferView::offsetOfVector()), T11);
}

static void getFromMegamorphicCache(CCallHelpers&, GPRReg uid, CCallHelpers::JumpList& notFound, GPRReg dataOfUntriedSlot = InvalidGPRReg);

static void fallThroughToNextStub(CCallHelpers& jit)
{
    while (jit.m_assembler.codeSize() % 16)
        jit.nop();
}

static void boxIntegerInA1(CCallHelpers& jit)
{
    jit.signExtend32ToPtr(A1, T11);
    Jump isInt32 = jit.branch64(CCallHelpers::Equal, A1, T11);
    jit.convertInt64ToDouble(A1, FPRInfo::fpRegT0);
    jit.moveDoubleTo64(FPRInfo::fpRegT0, A1);
    jit.sub64(numberTag, A1);
    Jump done = jit.jump();
    isInt32.link(&jit);
    jit.zeroExtend32ToWord(A1, A1);
    jit.or64(numberTag, A1);
    done.link(&jit);
}

static void generateAppendSpread(CCallHelpers& jit)
{
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    constexpr GPRReg target = A1;
    constexpr GPRReg source = A2;
    constexpr GPRReg length = R0;
    constexpr GPRReg capacity = T9;
    constexpr GPRReg allocator = T10;
    constexpr GPRReg count = T11;
    constexpr GPRReg scratch = T13;
#if CPU(ARM64)
    constexpr GPRReg old = T12;
    constexpr GPRReg storage = T14;
#else
    constexpr GPRReg old = A3;
    constexpr GPRReg storage = A0;
#endif
    constexpr GPRReg from = allocator;
    constexpr GPRReg element = capacity;
    constexpr unsigned maxCount = 64;
    constexpr unsigned maxCapacity = 255;
    static_assert(noOverlap(target, source, length, capacity, allocator, count, scratch, old, storage, instanceGPR));
    static_assert(sizeof(IndexingHeader) == sizeof(EncodedJSValue) && MarkedSpace::sizeStep == 2 * sizeof(EncodedJSValue) && sizeof(Allocator) == sizeof(void*));
    static_assert(sizeof(IndexingHeader) + maxCapacity * sizeof(EncodedJSValue) <= MarkedSpace::largeCutoff);
    static_assert(Butterfly::offsetOfPublicLength() == -static_cast<ptrdiff_t>(sizeof(IndexingHeader)) && Butterfly::offsetOfVectorLength() == Butterfly::offsetOfPublicLength() + static_cast<ptrdiff_t>(sizeof(uint32_t)));
    CCallHelpers::JumpList isLeftToOperation;
    CCallHelpers::JumpList hasRunOut;
    CCallHelpers::Label start = jit.label();
    isLeftToOperation.append(jit.branchIfNotCell(source));
    jit.load8(Address(source, JSCell::indexingTypeAndMiscOffset()), scratch);
    jit.move(scratch, count);
    jit.urshift32(TrustedImm32(Instance::arrayKindShift), count);
    jit.and32(TrustedImm32(Instance::numberOfArrayKinds - 1), count);
    jit.load32(CCallHelpers::BaseIndex(instanceGPR, count, CCallHelpers::TimesFour, Instance::offsetOfOriginalArrayStructureIDs()), count);
    isLeftToOperation.append(jit.branch32(CCallHelpers::NotEqual, count, Address(source, JSCell::structureIDOffset())));
    jit.and32(TrustedImm32(IndexingShapeMask), scratch);
    Jump keepsValues = jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(ContiguousShape));
    isLeftToOperation.append(jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(Int32Shape)));
    keepsValues.link(&jit);
    jit.load8(Address(target, JSCell::indexingTypeAndMiscOffset()), scratch);
    jit.and32(TrustedImm32(IndexingShapeAndWritabilityMask), scratch);
    isLeftToOperation.append(jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(ContiguousShape)));
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfVM()), scratch);
    isLeftToOperation.append(jit.branchTest8(CCallHelpers::NonZero, Address(scratch, VM::offsetOfHeapMutatorShouldBeFenced())));
    jit.load8(Address(target, JSCell::cellStateOffset()), count);
    isLeftToOperation.append(jit.branch32(CCallHelpers::BelowOrEqual, count, Address(scratch, VM::offsetOfHeapBarrierThreshold())));
    jit.loadPtr(Address(source, JSObject::butterflyOffset()), from);
    jit.load32(Address(from, Butterfly::offsetOfPublicLength()), count);
    isLeftToOperation.append(jit.branch32(CCallHelpers::Above, count, TrustedImm32(maxCount)));
    Jump isEmpty = jit.branchTest32(CCallHelpers::Zero, count);
    jit.loadPtr(Address(target, JSObject::butterflyOffset()), old);
    jit.load32(Address(old, Butterfly::offsetOfPublicLength()), length);
    jit.add32(length, count, capacity);
    Jump hasRoom = jit.branch32(CCallHelpers::BelowOrEqual, capacity, Address(old, Butterfly::offsetOfVectorLength()));

    jit.or32(TrustedImm32(1), capacity);
    isLeftToOperation.append(jit.branch32(CCallHelpers::Above, capacity, TrustedImm32(maxCapacity)));
    jit.add32(TrustedImm32(1), capacity, scratch);
    jit.urshift32(TrustedImm32(1), scratch);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfAuxiliarySpace()), allocator);
    jit.loadPtr(CCallHelpers::BaseIndex(allocator, scratch, CCallHelpers::TimesEight, CompleteSubspace::offsetOfAllocatorForSizeStep()), allocator);
    hasRunOut.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    jit.emitAllocateWithNonNullAllocator(storage, JITAllocator::variable(), allocator, scratch, hasRunOut, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
    jit.store32(length, Address(storage, 0));
    jit.store32(capacity, Address(storage, sizeof(uint32_t)));
    jit.store64(CCallHelpers::TrustedImm64(0), CCallHelpers::BaseIndex(storage, capacity, CCallHelpers::TimesEight));
    jit.addPtr(TrustedImm32(sizeof(IndexingHeader)), storage);
    jit.move(TrustedImm32(0), scratch);
    Jump isMoved = jit.branchTest32(CCallHelpers::Zero, length);
    CCallHelpers::Label move = jit.label();
    jit.load64(CCallHelpers::BaseIndex(old, scratch, CCallHelpers::TimesEight), element);
    jit.store64(element, CCallHelpers::BaseIndex(storage, scratch, CCallHelpers::TimesEight));
    jit.add32(TrustedImm32(1), scratch);
    jit.branch32(CCallHelpers::Below, scratch, length).linkTo(move, &jit);
    isMoved.link(&jit);
    jit.storePtr(storage, Address(target, JSObject::butterflyOffset()));
    jit.move(storage, old);

    hasRoom.link(&jit);
    jit.loadPtr(Address(source, JSObject::butterflyOffset()), from);
    jit.getEffectiveAddress(CCallHelpers::BaseIndex(old, length, CCallHelpers::TimesEight), storage);
    jit.move(TrustedImm32(0), scratch);
    CCallHelpers::Label copy = jit.label();
    jit.load64(CCallHelpers::BaseIndex(from, scratch, CCallHelpers::TimesEight), element);
    Jump isNoHole = jit.branchTest64(CCallHelpers::NonZero, element);
    jit.move(CCallHelpers::TrustedImm64(JSValue::ValueUndefined), element);
    isNoHole.link(&jit);
    jit.store64(element, CCallHelpers::BaseIndex(storage, scratch, CCallHelpers::TimesEight));
    jit.add32(TrustedImm32(1), scratch);
    jit.branch32(CCallHelpers::Below, scratch, count).linkTo(copy, &jit);
    jit.add32(count, length);
    jit.store32(length, Address(old, Butterfly::offsetOfPublicLength()));
    isEmpty.link(&jit);
    jit.move(target, R0);
    jit.ret();

    hasRunOut.link(&jit);
    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(2 * sizeof(EncodedJSValue)), sp);
    jit.store64(target, Address(sp, 0));
    jit.store64(source, Address(sp, sizeof(EncodedJSValue)));
    jit.move(capacity, A1);
    jit.lshift64(TrustedImm32(3), A1);
    jit.add64(TrustedImm32(sizeof(IndexingHeader)), A1);
    jit.move(instanceGPR, A0);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), operationGPR);
    jit.loadPtr(Address(operationGPR, static_cast<unsigned>(Entry::operationAOTTypedAllocateStorage) * sizeof(void*)), operationGPR);
    prepareCallOperation(jit, assertionScratchGPR);
    jit.call(operationGPR, OperationPtrTag);
    jit.load64(Address(sp, 0), target);
    jit.load64(Address(sp, sizeof(EncodedJSValue)), source);
    jit.emitFunctionEpilogue();
    jit.branchTestPtr(CCallHelpers::NonZero, R0).linkTo(start, &jit);

    isLeftToOperation.link(&jit);
    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(2 * sizeof(EncodedJSValue)), sp);
    jit.store64(target, Address(sp, 0));
    jit.store64(source, Address(sp, sizeof(EncodedJSValue)));
    jit.move(instanceGPR, A0);
    jit.move(TrustedImm32(static_cast<int32_t>(Typed::LanguageOperation::AppendSpread)), A1);
    jit.move(TrustedImm32(2), A2);
    jit.move(sp, A3);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), operationGPR);
    jit.loadPtr(Address(operationGPR, static_cast<unsigned>(Entry::operationAOTTypedLanguageOperation) * sizeof(void*)), operationGPR);
    prepareCallOperation(jit, assertionScratchGPR);
    jit.call(operationGPR, OperationPtrTag);
    jit.emitFunctionEpilogue();
    Jump exception = jit.branchTestPtr(CCallHelpers::NonZero, GPRInfo::returnValueGPR2);
    jit.ret();
    exception.link(&jit);
    loadInstance(jit, T9);
    jumpToEntry(jit, T9, Entry::HandleException);
}

static void generateTakeElements(CCallHelpers& jit)
{
    constexpr GPRReg taken = A1;
    constexpr GPRReg count = A2;
    constexpr GPRReg element = A3;
    constexpr GPRReg index = T9;
    constexpr GPRReg storage = T11;
    constexpr GPRReg length = T13;
    static_assert(noOverlap(R0, taken, count, element, index, storage, length, instanceGPR));
    CCallHelpers::JumpList isLeftToOperation;
    isLeftToOperation.append(jit.branchIfNotCell(R0));
    jit.load8(Address(R0, JSCell::indexingTypeAndMiscOffset()), length);
    jit.move(length, storage);
    jit.urshift32(TrustedImm32(Instance::arrayKindShift), storage);
    jit.and32(TrustedImm32(Instance::numberOfArrayKinds - 1), storage);
    jit.load32(CCallHelpers::BaseIndex(instanceGPR, storage, CCallHelpers::TimesFour, Instance::offsetOfOriginalArrayStructureIDs()), storage);
    isLeftToOperation.append(jit.branch32(CCallHelpers::NotEqual, storage, Address(R0, JSCell::structureIDOffset())));
    jit.and32(TrustedImm32(IndexingShapeMask), length);
    Jump keepsValues = jit.branch32(CCallHelpers::Equal, length, TrustedImm32(ContiguousShape));
    Jump keepsDoubles = jit.branch32(CCallHelpers::Equal, length, TrustedImm32(DoubleShape));
    isLeftToOperation.append(jit.branch32(CCallHelpers::NotEqual, length, TrustedImm32(Int32Shape)));
    keepsValues.link(&jit);
    jit.loadPtr(Address(R0, JSObject::butterflyOffset()), storage);
    jit.load32(Address(storage, Butterfly::offsetOfPublicLength()), length);
    jit.move(TrustedImm32(0), index);
    CCallHelpers::Label next = jit.label();
    jit.move(CCallHelpers::TrustedImm64(JSValue::ValueUndefined), element);
    Jump isPastEnd = jit.branch32(CCallHelpers::AboveOrEqual, index, length);
    jit.load64(CCallHelpers::BaseIndex(storage, index, CCallHelpers::TimesEight), element);
    isLeftToOperation.append(jit.branchTest64(CCallHelpers::Zero, element));
    isPastEnd.link(&jit);
    jit.store64(element, CCallHelpers::BaseIndex(taken, index, CCallHelpers::TimesEight));
    jit.add32(TrustedImm32(1), index);
    jit.branch32(CCallHelpers::NotEqual, index, count).linkTo(next, &jit);
    jit.ret();

    keepsDoubles.link(&jit);
    constexpr FPRReg number = FPRInfo::fpRegT0;
    jit.loadPtr(Address(R0, JSObject::butterflyOffset()), storage);
    jit.load32(Address(storage, Butterfly::offsetOfPublicLength()), length);
    jit.move(TrustedImm32(0), index);
    CCallHelpers::Label nextDouble = jit.label();
    jit.move(CCallHelpers::TrustedImm64(JSValue::ValueUndefined), element);
    Jump isPastEndOfDoubles = jit.branch32(CCallHelpers::AboveOrEqual, index, length);
    jit.loadDouble(CCallHelpers::BaseIndex(storage, index, CCallHelpers::TimesEight), number);
    isLeftToOperation.append(jit.branchIfNaN(number));
    CCallHelpers::JumpList isNoInt32;
    branchIfNoExactInt32(jit, number, element, isNoInt32, FPRInfo::fpRegT1);
    jit.zeroExtend32ToWord(element, element);
    jit.add64(numberTag, element);
    Jump isBoxed = jit.jump();
    isNoInt32.link(&jit);
    jit.moveDoubleTo64(number, element);
    jit.sub64(numberTag, element);
    isBoxed.link(&jit);
    isPastEndOfDoubles.link(&jit);
    jit.store64(element, CCallHelpers::BaseIndex(taken, index, CCallHelpers::TimesEight));
    jit.add32(TrustedImm32(1), index);
    jit.branch32(CCallHelpers::NotEqual, index, count).linkTo(nextDouble, &jit);
    jit.ret();

    isLeftToOperation.link(&jit);
    jit.move(taken, A3);
    jit.move(R0, A1);
    jit.move(TrustedImm32(static_cast<unsigned>(Entry::operationAOTTypedTakeElements) * sizeof(void*)), T9);
    jit.jump().linkTo(stubLabels()[static_cast<unsigned>(Stub::OperationVoidWithInstance)], &jit);
}

static void generateGetByValAtIndex(CCallHelpers& jit)
{
    static_assert(static_cast<unsigned>(Stub::GetByValAtIndex) + 1 == static_cast<unsigned>(Stub::GetByVal));
    CCallHelpers::JumpList otherwise;
    otherwise.append(jit.branchIfNotCell(R0));
    jit.load8(Address(R0, JSCell::indexingTypeAndMiscOffset()), T13);
    jit.and32(TrustedImm32(IndexingShapeMask), T13);
    Jump contiguousCase = jit.branch32(CCallHelpers::Equal, T13, TrustedImm32(ContiguousShape));
    otherwise.append(jit.branch32(CCallHelpers::NotEqual, T13, TrustedImm32(Int32Shape)));
    contiguousCase.link(&jit);
    jit.loadPtr(Address(R0, JSObject::butterflyOffset()), T11);
    jit.load32(Address(T11, Butterfly::offsetOfPublicLength()), T12);
    otherwise.append(jit.branch64(CCallHelpers::AboveOrEqual, A1, T12));
    jit.load64(CCallHelpers::BaseIndex(T11, A1, CCallHelpers::TimesEight), T11);
    otherwise.append(jit.branchTest64(CCallHelpers::Zero, T11));
    jit.move(T11, R0);
    jit.ret();

    otherwise.link(&jit);
    boxIntegerInA1(jit);
    fallThroughToNextStub(jit);
}

static void generateGetByVal(CCallHelpers& jit)
{
    constexpr FPRReg number = FPRInfo::fpRegT0;
    CCallHelpers::JumpList slow;
    CCallHelpers::JumpList absent;
    Jump notInt32 = jit.branch64(CCallHelpers::Below, A1, numberTag);
    CCallHelpers::Label indexReady = jit.label();
    slow.append(jit.branchIfNotCell(R0));
    jit.load8(Address(R0, JSCell::indexingTypeAndMiscOffset()), T13);
    jit.and32(TrustedImm32(IndexingShapeMask), T13);
    static_assert(JSObject::butterflyOffset() + sizeof(void*) <= MarkedBlock::atomSize);
    jit.loadPtr(Address(R0, JSObject::butterflyOffset()), T11);
    jit.zeroExtend32ToWord(A1, T12);
    Jump isContiguous = jit.branch32(CCallHelpers::Equal, T13, TrustedImm32(ContiguousShape));
    Jump noButterfly = jit.branchTest32(CCallHelpers::Zero, T13);
    Jump doubleCase = jit.branch32(CCallHelpers::Equal, T13, TrustedImm32(DoubleShape));
    absent.append(jit.branch32(CCallHelpers::Equal, T13, TrustedImm32(UndecidedShape)));
    slow.append(jit.branch32(CCallHelpers::NotEqual, T13, TrustedImm32(Int32Shape)));
    isContiguous.link(&jit);

    absent.append(jit.branch32(CCallHelpers::AboveOrEqual, T12, Address(T11, Butterfly::offsetOfPublicLength())));
    jit.load64(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesEight), T11);
    absent.append(jit.branchTest64(CCallHelpers::Zero, T11));
    jit.move(T11, R0);
    jit.ret();

    auto boxInt32AndReturn = [&] {
        jit.add64(numberTag, T11, R0);
        jit.ret();
    };
    auto boxDoubleAndReturn = [&] {
        CCallHelpers::JumpList isNoInt32;
        branchIfNoExactInt32(jit, number, T11, isNoInt32, FPRInfo::fpRegT1);
        boxInt32AndReturn();
        isNoInt32.link(&jit);
        jit.moveDoubleTo64(number, R0);
        jit.sub64(numberTag, R0);
        jit.ret();
    };

    doubleCase.link(&jit);
    absent.append(jit.branch32(CCallHelpers::AboveOrEqual, T12, Address(T11, Butterfly::offsetOfPublicLength())));
    jit.loadDouble(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesEight), number);
    absent.append(jit.branchIfNaN(number));
    boxDoubleAndReturn();

    // From its length on and in a hole an array as the engine makes them has no element, and inherits none: see the head of AOTTypedLoweringContainers.cpp. A negative number is a name.
    absent.link(&jit);
    slow.append(jit.branch8(CCallHelpers::NotEqual, Address(R0, JSCell::typeInfoTypeOffset()), TrustedImm32(ArrayType)));
    slow.append(jit.branch32(CCallHelpers::LessThan, A1, TrustedImm32(0)));
    jit.move(CCallHelpers::TrustedImm64(JSValue::ValueUndefined), R0);
    jit.ret();

    noButterfly.link(&jit);
    Jump notString = jit.branchIfNotString(R0);
    jit.loadPtr(Address(R0, JSString::offsetOfValue()), T11);
    slow.append(jit.branchIfRopeStringImpl(T11));
    jit.zeroExtend32ToWord(A1, T12);
    slow.append(jit.branch32(CCallHelpers::AboveOrEqual, T12, Address(T11, StringImpl::lengthMemoryOffset())));
    jit.load32(Address(T11, StringImpl::flagsOffset()), T13);
    jit.loadPtr(Address(T11, StringImpl::dataOffset()), T11);
    Jump is16Bit = jit.branchTest32(CCallHelpers::Zero, T13, TrustedImm32(StringImpl::flagIs8Bit()));
    jit.load8(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesOne), T11);
    Jump characterReady = jit.jump();
    is16Bit.link(&jit);
    jit.load16(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesTwo), T11);
    slow.append(jit.branch32(CCallHelpers::Above, T11, TrustedImm32(maxSingleCharacterString)));
    characterReady.link(&jit);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfVM()), T12);
    jit.addPtr(TrustedImm32(OBJECT_OFFSETOF(VM, smallStrings) + SmallStrings::offsetOfSingleCharacterStrings()), T12);
    jit.loadPtr(CCallHelpers::BaseIndex(T12, T11, CCallHelpers::TimesEight), R0);
    jit.ret();

    notString.link(&jit);
    checkTypedArrayAccess(jit, slow);
    auto forJSType = [&](JSType type, const auto& load) {
        Jump other = jit.branch32(CCallHelpers::NotEqual, T13, TrustedImm32(type));
        load();
        other.link(&jit);
    };
    forJSType(Int32ArrayType, [&] {
        jit.load32(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesFour), T11);
        boxInt32AndReturn();
    });
    forJSType(Uint8ArrayType, [&] {
        jit.load8(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesOne), T11);
        boxInt32AndReturn();
    });
    forJSType(Float64ArrayType, [&] {
        jit.loadDouble(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesEight), number);
        jit.purifyNaN(number, number);
        boxDoubleAndReturn();
    });
    forJSType(Uint8ClampedArrayType, [&] {
        jit.load8(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesOne), T11);
        boxInt32AndReturn();
    });
    forJSType(Uint16ArrayType, [&] {
        jit.load16(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesTwo), T11);
        boxInt32AndReturn();
    });
    forJSType(Int16ArrayType, [&] {
        jit.load16SignedExtendTo32(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesTwo), T11);
        boxInt32AndReturn();
    });
    forJSType(Int8ArrayType, [&] {
        jit.load8SignedExtendTo32(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesOne), T11);
        boxInt32AndReturn();
    });
    forJSType(Uint32ArrayType, [&] {
        jit.load32(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesFour), T11);
        Jump isInt32 = jit.branch32(CCallHelpers::GreaterThanOrEqual, T11, TrustedImm32(0));
        jit.convertInt64ToDouble(T11, number);
        boxDoubleAndReturn();
        isInt32.link(&jit);
        boxInt32AndReturn();
    });
    forJSType(Float32ArrayType, [&] {
        jit.loadFloat(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesFour), number);
        jit.convertFloatToDouble(number, number);
        jit.purifyNaN(number, number);
        boxDoubleAndReturn();
    });

    notInt32.link(&jit);
    Jump notNumber = jit.branchTest64(CCallHelpers::Zero, A1, numberTag);
    jit.add64(numberTag, A1, T11);
    jit.move64ToDouble(T11, number);
    jit.branchConvertDoubleToInt32(number, T11, slow, FPRInfo::fpRegT1, false);
    jit.add64(numberTag, T11, A1);
    jit.jump().linkTo(indexReady, &jit);

    notNumber.link(&jit);
    slow.append(jit.branchIfNotCell(R0));
    slow.append(jit.branchIfNotObject(R0));
    slow.append(jit.branchIfNotCell(A1));
    Jump isSymbol = jit.branchIfSymbol(A1);
    slow.append(jit.branchIfNotString(A1));
    jit.loadPtr(Address(A1, JSString::offsetOfValue()), A2);
    slow.append(jit.branchIfRopeStringImpl(A2));
    slow.append(jit.branchTest32(CCallHelpers::Zero, Address(A2, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIsAtom())));
    Jump nameReady = jit.jump();
    isSymbol.link(&jit);
    jit.loadPtr(Address(A1, Symbol::offsetOfSymbolImpl()), A2);
    nameReady.link(&jit);
    getFromMegamorphicCache(jit, A2, slow);

    slow.link(&jit);
    callBinaryOperation(jit, Entry::operationAOTGetByVal);
}

static void generatePutByValAtIndex(CCallHelpers& jit)
{
    static_assert(static_cast<unsigned>(Stub::PutByValAtIndex) + 1 == static_cast<unsigned>(Stub::PutByVal));
    CCallHelpers::JumpList otherwise;
    otherwise.append(jit.branchIfNotCell(R0));
    otherwise.append(jit.branch8(CCallHelpers::NotEqual, Address(R0, JSCell::typeInfoTypeOffset()), TrustedImm32(ArrayType)));
    jit.load8(Address(R0, JSCell::indexingTypeAndMiscOffset()), T11);
    jit.and32(TrustedImm32(IndexingShapeMask | CopyOnWrite | MayHaveIndexedAccessors), T11);
    otherwise.append(jit.branch32(CCallHelpers::NotEqual, T11, TrustedImm32(ContiguousShape)));
    jit.loadPtr(Address(R0, JSObject::butterflyOffset()), T11);
    jit.load32(Address(T11, Butterfly::offsetOfPublicLength()), T12);
    otherwise.append(jit.branch64(CCallHelpers::AboveOrEqual, A1, T12));
    jit.store64(A2, CCallHelpers::BaseIndex(T11, A1, CCallHelpers::TimesEight));
    returnAfterStoring(jit, A2);

    otherwise.link(&jit);
    boxIntegerInA1(jit);
    fallThroughToNextStub(jit);
}

static void generatePutByVal(CCallHelpers& jit)
{
    CCallHelpers::JumpList slow;
    slow.append(jit.branch64(CCallHelpers::Below, A1, numberTag));
    slow.append(jit.branchIfNotCell(R0));
    jit.load8(Address(R0, JSCell::indexingTypeAndMiscOffset()), T11);
    jit.and32(TrustedImm32(IndexingShapeMask | CopyOnWrite), T11);
    Jump isContiguous = jit.branch32(CCallHelpers::Equal, T11, TrustedImm32(ContiguousShape));
    Jump isNotInt32Shape = jit.branch32(CCallHelpers::NotEqual, T11, TrustedImm32(Int32Shape));
    slow.append(jit.branch64(CCallHelpers::Below, A2, numberTag));
    Jump staysInt32Shape = jit.jump();
    isNotInt32Shape.link(&jit);
    Jump isDoubleShape = jit.branch32(CCallHelpers::Equal, T11, TrustedImm32(DoubleShape));
    slow.append(jit.branchTest32(CCallHelpers::NonZero, T11));

    {
        constexpr FPRReg number = FPRInfo::fpRegT0;
        checkTypedArrayAccess(jit, slow);
        Jump valueIsNotInt32 = jit.branch64(CCallHelpers::Below, A2, numberTag);
        auto forJSType = [&](JSType type, const auto& store) {
            Jump other = jit.branch32(CCallHelpers::NotEqual, T13, TrustedImm32(type));
            store();
            jit.ret();
            other.link(&jit);
        };
        auto store32 = [&] { jit.store32(A2, CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesFour)); };
        auto store16 = [&] { jit.store16(A2, CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesTwo)); };
        auto store8 = [&] { jit.store8(A2, CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesOne)); };
        auto storeDouble = [&] { jit.storeDouble(number, CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesEight)); };
        auto storeFloat = [&] {
            jit.convertDoubleToFloat(number, number);
            jit.storeFloat(number, CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesFour));
        };
        forJSType(Int32ArrayType, store32);
        forJSType(Uint8ArrayType, store8);
        forJSType(Uint32ArrayType, store32);
        forJSType(Uint16ArrayType, store16);
        forJSType(Int16ArrayType, store16);
        forJSType(Int8ArrayType, store8);
        jit.convertInt32ToDouble(A2, number);
        forJSType(Float64ArrayType, storeDouble);
        forJSType(Float32ArrayType, storeFloat);
        slow.append(jit.jump());

        valueIsNotInt32.link(&jit);
        slow.append(jit.branchTest64(CCallHelpers::Zero, A2, numberTag));
        jit.add64(numberTag, A2, A4);
        jit.move64ToDouble(A4, number);
        forJSType(Float64ArrayType, storeDouble);
        forJSType(Float32ArrayType, storeFloat);
        slow.append(jit.jump());

        isDoubleShape.link(&jit);
        Jump isInt32 = jit.branch64(CCallHelpers::AboveOrEqual, A2, numberTag);
        slow.append(jit.branchTest64(CCallHelpers::Zero, A2, numberTag));
        jit.add64(numberTag, A2, A4);
        jit.move64ToDouble(A4, number);
        slow.append(jit.branchIfNaN(number));
        Jump isReady = jit.jump();
        isInt32.link(&jit);
        jit.convertInt32ToDouble(A2, number);
        isReady.link(&jit);
        jit.loadPtr(Address(R0, JSObject::butterflyOffset()), T11);
        jit.zeroExtend32ToWord(A1, T12);
        Jump isWithinLength = jit.branch32(CCallHelpers::Below, T12, Address(T11, Butterfly::offsetOfPublicLength()));
        slow.append(jit.branch32(CCallHelpers::AboveOrEqual, T12, Address(T11, Butterfly::offsetOfVectorLength())));
        jit.add32(TrustedImm32(1), T12, T13);
        jit.store32(T13, Address(T11, Butterfly::offsetOfPublicLength()));
        isWithinLength.link(&jit);
        storeDouble();
        jit.ret();
    }

    isContiguous.link(&jit);
    staysInt32Shape.link(&jit);
    jit.loadPtr(Address(R0, JSObject::butterflyOffset()), T11);
    jit.zeroExtend32ToWord(A1, T12);
    Jump inBounds = jit.branch32(CCallHelpers::Below, T12, Address(T11, Butterfly::offsetOfPublicLength()));
    slow.append(jit.branch32(CCallHelpers::AboveOrEqual, T12, Address(T11, Butterfly::offsetOfVectorLength())));
    jit.add32(TrustedImm32(1), T12, T13);
    jit.store32(T13, Address(T11, Butterfly::offsetOfPublicLength()));
    inBounds.link(&jit);
    jit.store64(A2, CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesEight));

    returnAfterStoring(jit, A2);

    slow.link(&jit);
    loadInstance(jit, T11);
    jit.move(A3, A4);
    jit.move(A2, A3);
    jit.move(A1, A2);
    jit.move(R0, A1);
    jit.move(T11, A0);
    jit.loadPtr(Address(T11, Instance::offsetOfRuntimeTable()), T11);
    jit.loadPtr(Address(T11, static_cast<unsigned>(Entry::operationAOTPutByVal) * sizeof(void*)), T11);
    callAndCheckException(jit, T11, Returns::Void);
}

static Address slotWord(GPRReg slot, unsigned word) { return Address(slot, word * sizeof(void*)); }

static Jump branchIfStructureDiffers(CCallHelpers& jit, GPRReg cell, GPRReg firstSlotWord)
{
#if CPU(X86_64)
    return jit.branch32(CCallHelpers::NotEqual, firstSlotWord, Address(cell, JSCell::structureIDOffset()));
#else
    jit.load32(Address(cell, JSCell::structureIDOffset()), T12);
    return jit.branch32(CCallHelpers::NotEqual, firstSlotWord, T12);
#endif
}

template<uint32_t flag>
static Jump branchIfSlotHas(CCallHelpers& jit, GPRReg firstSlotWord)
{
    static_assert(hasOneBitSet(flag));
#if CPU(X86_64)
    return jit.branchTestBit64(CCallHelpers::NonZero, firstSlotWord, TrustedImm32(32 + getLSBSet(flag)));
#else
    return jit.branchTest64(CCallHelpers::NonZero, firstSlotWord, CCallHelpers::TrustedImm64(static_cast<int64_t>(static_cast<uint64_t>(flag) << 32)));
#endif
}

template<uint32_t flag>
static Jump branchUnlessSlotHas(CCallHelpers& jit, GPRReg firstSlotWord)
{
    static_assert(hasOneBitSet(flag));
#if CPU(X86_64)
    return jit.branchTestBit64(CCallHelpers::Zero, firstSlotWord, TrustedImm32(32 + getLSBSet(flag)));
#else
    return jit.branchTest64(CCallHelpers::Zero, firstSlotWord, CCallHelpers::TrustedImm64(static_cast<int64_t>(static_cast<uint64_t>(flag) << 32)));
#endif
}

static void loadDirectLocation(CCallHelpers& jit, GPRReg slot, GPRReg firstSlotWord, GPRReg result)
{
#if CPU(X86_64)
    static_assert(Slot::directLocationBits == 8);
    UNUSED_PARAM(firstSlotWord);
    jit.load8(Address(slot, sizeof(uint32_t)), result);
#else
    UNUSED_PARAM(slot);
    extractBits(jit, firstSlotWord, 32, Slot::directLocationBits, result);
#endif
}

static void slotSite(CCallHelpers& jit, GPRReg data, GPRReg slot, GPRReg result)
{
    static_assert(sizeof(Slot) == 16);
    jit.subPtr(slot, data, result);
    jit.subPtr(TrustedImm32(static_cast<int32_t>(Data::offsetOfSlots() - BiasedDataPointer::bias)), result);
    jit.urshiftPtr(TrustedImm32(4), result);
}

static CCallHelpers::BaseIndex siteOfSlotIn(CCallHelpers& jit, GPRReg data, GPRReg slot, GPRReg sites, GPRReg index)
{
    static_assert(sizeof(Slot) == 16 && sizeof(Site) == 4);
#if CPU(X86_64)
    RELEASE_ASSERT(!(Data::offsetOfSlots() % sizeof(Slot)));
    jit.subPtr(slot, data, index);
    jit.addPtr(TrustedImm32(BiasedDataPointer::bias), index);
    jit.urshiftPtr(TrustedImm32(4), index);
    return CCallHelpers::BaseIndex(sites, index, CCallHelpers::TimesFour, -static_cast<int32_t>(Data::offsetOfSlots() / sizeof(Slot) * sizeof(Site)));
#else
    slotSite(jit, data, slot, index);
    return CCallHelpers::BaseIndex(sites, index, CCallHelpers::TimesFour);
#endif
}

static void prepareMissAtSite(CCallHelpers& jit, Entry operation, unsigned numberOfOperands, unsigned bytesPushed = 0, CCallHelpers::JumpList* hasCallerIndex = nullptr, bool callerIsThunk = false)
{
    RELEASE_ASSERT(numberOfOperands >= 1 && numberOfOperands <= 3);
    constexpr GPRReg arguments[] = { operationArgumentGPR(0), operationArgumentGPR(1), operationArgumentGPR(2), operationArgumentGPR(3), operationArgumentGPR(4), operationArgumentGPR(5), operationArgumentGPR(6) };
    GPRReg site = arguments[numberOfOperands];
    loadCallerIndex(jit, bytesPushed, callerIsThunk);
    if (hasCallerIndex)
        hasCallerIndex->link(&jit);
    loadDataForSlot(jit, site, T10);
    CCallHelpers::BaseIndex siteOfSlot = siteOfSlotIn(jit, T10, site, T11, T13);
    loadInfo(jit, T11);
    loadSites(jit, T11, T11);
    static_assert(sizeof(Site) == 4);
    jit.load32(siteOfSlot, T12);
    jit.move(site, T11);

    for (unsigned i = numberOfOperands; i--;)
        jit.move(i ? arguments[i] : R0, arguments[i + 1]);
    GPRReg identifier = arguments[numberOfOperands + 1];
    GPRReg slot = arguments[numberOfOperands + 2];
    GPRReg extra = arguments[numberOfOperands + 3];
    ASSERT(identifier != T11 && identifier != T12 && slot != T12);
    jit.and32(TrustedImm32((1u << Site::identifierBits) - 1), T12, identifier);
    jit.move(T11, slot);
    jit.urshift32(T12, TrustedImm32(Site::identifierBits), extra);
    jit.move(instanceGPR, A0);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T9);
    jit.loadPtr(Address(T9, static_cast<unsigned>(operation) * sizeof(void*)), T9);
}

static void missAtSite(CCallHelpers& jit, Entry operation, unsigned numberOfOperands, Returns returns, CCallHelpers::JumpList* hasCallerIndex = nullptr, bool callerIsThunk = false)
{
    prepareMissAtSite(jit, operation, numberOfOperands, 0, hasCallerIndex, callerIsThunk);
    callAndCheckException(jit, T9, returns, GPRInfo::returnValueGPR, callerIsThunk);
}

static void locateCachedProperty(CCallHelpers& jit, GPRReg object, GPRReg word, GPRReg storage)
{
#if CPU(ARM64)
    jit.extractSignedBitfield64(word, TrustedImm32(32), TrustedImm32(Slot::offsetBits), word);
#else
    jit.lshift64(word, TrustedImm32(32 - Slot::offsetBits), word);
    jit.rshift64(word, TrustedImm32(64 - Slot::offsetBits), word);
#endif
    jit.loadPtr(Address(object, JSObject::butterflyOffset()), storage);
#if CPU(X86_64)
    static_assert(Slot::offsetBits < 64);
    ASSERT(noOverlap(object, word, storage));
    jit.m_assembler.cmovq_rr(X86Assembler::ConditionNS, object, storage);
#else
    jit.moveConditionally64(CCallHelpers::LessThan, word, TrustedImm32(0), storage, object, storage);
#endif
}

static Address outgoingFrameSlot(CallFrameSlot slot, ptrdiff_t offset = 0)
{
    return Address(CCallHelpers::stackPointerRegister, (static_cast<int>(slot) - CallerFrameAndPC::sizeInRegisters) * static_cast<int>(sizeof(Register)) + offset);
}

static Address incomingFrameSlot(CallFrameSlot slot, ptrdiff_t offset = 0)
{
    return Address(CCallHelpers::stackPointerRegister, static_cast<int>(slot) * static_cast<int>(sizeof(Register)) - static_cast<int>(prologueStackPointerDelta()) + offset);
}

static void generateInstanceOfCached(CCallHelpers& jit)
{
    CCallHelpers::JumpList miss;
    miss.append(jit.branchIfNotCell(A1));
    jit.load64(slotWord(A2, 0), T11);
    miss.append(branchIfStructureDiffers(jit, A1, T11));
    Jump isNotCell = jit.branchIfNotCell(R0);
    Jump isNotObject = jit.branchIfNotObject(R0);
    locateCachedProperty(jit, A1, T11, T13);
    jit.load64(CCallHelpers::BaseIndex(T13, T11, CCallHelpers::TimesEight), A1);
    jit.jump().linkTo(stubLabels()[static_cast<unsigned>(Stub::InstanceOf)], &jit);

    isNotCell.link(&jit);
    isNotObject.link(&jit);
    jit.move(TrustedImm32(0), R0);
    jit.ret();

    miss.link(&jit);
    missAtSite(jit, Entry::operationAOTInstanceofAndCache, 2, Returns::Value);
}

static void checkStackBeforeCallingAccessor(CCallHelpers& jit)
{
    Jump fits = jit.branchPtr(CCallHelpers::AboveOrEqual, CCallHelpers::stackPointerRegister, Address(instanceGPR, Instance::offsetOfSoftStackLimit()));
    s_callsBetweenStubs->append({ jit.nearTailCall(), Stub::ThrowStackOverflow });
    fits.link(&jit);
}

static CCallHelpers::Label& variadicCallCellLabel();

static void callGetter(CCallHelpers& jit, CCallHelpers::JumpList& cannot)
{
    {
        jit.loadPtr(Address(T12, GetterSetter::offsetOfGetterShortcut()), T13);
        Jump hasNoShortcut = jit.branchTestPtr(CCallHelpers::Zero, T13);
        static_assert(calleeGPR == T10);
        jit.loadPtr(Address(T12, GetterSetter::offsetOfGetter()), calleeGPR);
        jit.load32(Address(calleeGPR, JSCell::structureIDOffset()), T11);
        structureWithID(jit, T11);
        Jump belongsToThisInstance = jit.branchPtr(CCallHelpers::Equal, Address(T11, Structure::offsetOfAOTInstance()), instanceGPR);
        Jump belongsToAnotherInstance = jit.branchTestPtr(CCallHelpers::NonZero, Address(T11, Structure::offsetOfAOTInstance()));
        jit.loadPtr(Address(T11, Structure::realmOffset()), T11);
        Jump belongsToAnotherRealm = jit.branchPtr(CCallHelpers::NotEqual, Address(instanceGPR, Instance::offsetOfGlobalObject()), T11);
        belongsToThisInstance.link(&jit);
        checkStackBeforeCallingAccessor(jit);
        jit.move(R0, thisGPR);
        jit.subPtr(TrustedImm32(GetterSetter::getterShortcutIsCode), T13);
        jit.farJump(T13, JSEntryPtrTag);

        hasNoShortcut.link(&jit);
        belongsToAnotherInstance.link(&jit);
        belongsToAnotherRealm.link(&jit);
    }
    jit.loadPtr(Address(T12, GetterSetter::offsetOfGetter()), T12);
    {
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfTypedArrayLengthGetter()), T13);
        Jump isDifferentGetter = jit.branchPtr(CCallHelpers::NotEqual, T12, T13);
        jit.load8(Address(R0, JSCell::typeInfoTypeOffset()), T13);
        jit.sub32(TrustedImm32(FirstTypedArrayType), T13);
        Jump isNotTypedArray = jit.branch32(CCallHelpers::AboveOrEqual, T13, TrustedImm32(NumberOfTypedArrayTypesExcludingDataView));
        jit.load8(Address(R0, JSArrayBufferView::offsetOfMode()), T13);
        Jump needsComputation = jit.branchTest32(CCallHelpers::NonZero, T13, TrustedImm32(resizabilityAndAutoLengthMask));
        jit.load64(Address(R0, JSArrayBufferView::offsetOfLength()), T13);
        Jump isTooLong = jit.branch64(CCallHelpers::Above, T13, CCallHelpers::TrustedImm64(std::numeric_limits<int32_t>::max()));
        jit.add64(numberTag, T13, R0);
        jit.ret();
        isDifferentGetter.link(&jit);
        isNotTypedArray.link(&jit);
        needsComputation.link(&jit);
        isTooLong.link(&jit);
    }
    {
        Jump isFunction = jit.branchTest8(CCallHelpers::NonZero, Address(T12, JSCell::typeInfoFlagsOffset()), TrustedImm32(TypeOfYieldsFunction));
        auto loadFunction = [&] {
            jit.load64(Address(R0, Typed::SlimClosure::offsetOfWord()), T12);
            jit.urshift64(TrustedImm32(ClosureWord::functionShift), T12);
            jit.and32(TrustedImm32(ClosureWord::functionMask), T12);
            jit.loadPtr(Address(instanceGPR, Instance::offsetOfTypedData()), T13);
        };
        auto markLazyPropertyRead = [&] {
            jit.load8(Address(R0, JSCell::typeInfoFlagsOffset()), T13);
            cannot.append(jit.branchTest32(CCallHelpers::NonZero, T13, TrustedImm32(TypeInfoPerCellBit)));
            jit.or32(TrustedImm32(TypeInfoPerCellBit), T13);
            jit.store8(T13, Address(R0, JSCell::typeInfoFlagsOffset()));
        };
        Jump isNotLength = jit.branchPtr(CCallHelpers::NotEqual, T12, Address(instanceGPR, Instance::offsetOfTypedSlimClosureLengthGetter()));
        markLazyPropertyRead();
        loadFunction();
        jit.load32(Address(T13, OBJECT_OFFSETOF(Typed::Data, functionLengthsOffset)), T11);
        jit.addPtr(T11, T13);
        if (s_functionLengthsAreBytes)
            jit.load8(CCallHelpers::BaseIndex(T13, T12, CCallHelpers::TimesOne), T13);
        else
            jit.load16(CCallHelpers::BaseIndex(T13, T12, CCallHelpers::TimesTwo), T13);
        jit.add64(numberTag, T13, R0);
        jit.ret();
        isNotLength.link(&jit);
        cannot.append(jit.branchPtr(CCallHelpers::NotEqual, T12, Address(instanceGPR, Instance::offsetOfTypedSlimClosureNameGetter())));
        markLazyPropertyRead();
        loadFunction();
        jit.load32(Address(T13, OBJECT_OFFSETOF(Typed::Data, functionsOffset)), T11);
        jit.addPtr(T11, T13);
        jit.load32(CCallHelpers::BaseIndex(T13, T12, CCallHelpers::TimesFour), T9);
        jit.add32(TrustedImm32(1), T9);
        s_callsBetweenStubs->append({ jit.nearTailCall(), Stub::TypedString });
        isFunction.link(&jit);
    }
    checkStackBeforeCallingAccessor(jit);
    jit.move(R0, thisGPR);
    jit.move(T12, calleeGPR);
    jit.move(TrustedImm32(0), countGPR);
    jit.jump().linkTo(variadicCallCellLabel(), &jit);
}

static void getFromMegamorphicCache(CCallHelpers& jit, GPRReg uid, CCallHelpers::JumpList& notFound, GPRReg dataOfUntriedSlot)
{
#if CPU(X86_64)
    constexpr GPRReg cache = T10;
    constexpr GPRReg result = T9;
#else
    constexpr GPRReg cache = GPRInfo::argumentGPR7;
    constexpr GPRReg result = GPRInfo::argumentGPR6;
#endif
    ASSERT(uid != cache && uid != result && uid != R0 && uid != A1);
    ASSERT(dataOfUntriedSlot == InvalidGPRReg || noOverlap(dataOfUntriedSlot, A1, cache, result, uid, T11, T12, T13));
    auto countAttempt = [&](bool mayFill) {
        if (dataOfUntriedSlot == InvalidGPRReg)
            return;
        using Entry = MegamorphicCache::LoadEntry;
        Jump isNone = jit.branchTestPtr(CCallHelpers::Zero, dataOfUntriedSlot);
        CCallHelpers::JumpList isLeftToTheOperation;
        Jump isFilled;
        if (mayFill) {
            constexpr GPRReg location = T11;
            constexpr GPRReg pointer = T12;
            constexpr uint32_t firstInlineLocation = JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue);
            isLeftToTheOperation.append(jit.branchTest8(CCallHelpers::Zero, Address(dataOfUntriedSlot, Data::offsetOfHasBeenFilledSinceLastCollection() - BiasedDataPointer::bias)));
            isLeftToTheOperation.append(jit.branchTest32(CCallHelpers::NonZero, Address(A1, OBJECT_OFFSETOF(Slot, offset))));
            jit.loadPtr(Address(T13, Entry::offsetOfHolder()), pointer);
            jit.load16(Address(T13, Entry::offsetOfOffset()), location);
            jit.load16(Address(T13, Entry::offsetOfEpoch()), cache);
            CCallHelpers::JumpList isReady;
            Jump isForGood = jit.branch32(CCallHelpers::Equal, cache, TrustedImm32(MegamorphicCache::permanentEpoch));
            isLeftToTheOperation.append(jit.branch64(CCallHelpers::NotEqual, pointer, TrustedImm32(static_cast<int32_t>(std::bit_cast<uintptr_t>(JSCell::seenMultipleCalleeObjects())))));
            Jump isOwnOutOfLine = jit.branch32(CCallHelpers::AboveOrEqual, location, TrustedImm32(firstOutOfLineOffset));
            jit.add32(TrustedImm32(firstInlineLocation), location);
            jit.move(uid, pointer);
            isReady.append(jit.jump());
            isOwnOutOfLine.link(&jit);
            jit.neg32(location);
            jit.add32(TrustedImm32(firstOutOfLineOffset - 2), location);
            jit.and32(TrustedImm32(Slot::offsetMask), location);
            jit.or32(TrustedImm32(Slot::isIndirect), location);
            jit.move(CCallHelpers::TrustedImmPtr(nullptr), pointer);
            isReady.append(jit.jump());
            isForGood.link(&jit);
            Jump isPresent = jit.branchTestPtr(CCallHelpers::NonZero, pointer);
            loadInstance(jit, pointer);
            jit.loadPtr(Address(pointer, Instance::offsetOfRuntimeTable()), pointer);
            jit.loadPtr(Address(pointer, static_cast<unsigned>(AOT::Entry::UndefinedHolder) * sizeof(void*)), pointer);
            jit.move(TrustedImm32(firstInlineLocation | Slot::isIndirect | Slot::pointerIsNotCell), location);
            isReady.append(jit.jump());

            isPresent.link(&jit);
            Jump isOutOfLine = jit.branch32(CCallHelpers::AboveOrEqual, location, TrustedImm32(firstOutOfLineOffset));
            jit.add32(TrustedImm32(firstInlineLocation), location);
            Jump isInLine = jit.jump();
            isOutOfLine.link(&jit);
            jit.neg32(location);
            jit.add32(TrustedImm32(firstOutOfLineOffset - 2), location);
            jit.and32(TrustedImm32(Slot::offsetMask), location);
            isInLine.link(&jit);
            jit.or32(TrustedImm32(Slot::isIndirect | Slot::pointerIsCell), location);

            isReady.link(&jit);
            jit.store32(location, Address(A1, OBJECT_OFFSETOF(Slot, offset)));
            jit.storePtr(pointer, Address(A1, OBJECT_OFFSETOF(Slot, pointer)));
            jit.storeFence();
            jit.load32(Address(R0, JSCell::structureIDOffset()), T11);
            jit.store32(T11, Address(A1, OBJECT_OFFSETOF(Slot, structureID)));
            jit.add64(TrustedImm32(1), Address(dataOfUntriedSlot, Data::offsetOfSlotEpoch() - BiasedDataPointer::bias));
            isFilled = jit.jump();
            isLeftToTheOperation.link(&jit);
        }
        jit.load32(Address(A1, OBJECT_OFFSETOF(Slot, offset)), T11);
        jit.add32(TrustedImm32(1u << Slot::attemptsShift), T11);
        jit.store32(T11, Address(A1, OBJECT_OFFSETOF(Slot, offset)));
        if (mayFill)
            isFilled.link(&jit);
        isNone.link(&jit);
    };
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), cache);
    jit.loadPtr(Address(cache, static_cast<unsigned>(Entry::MegamorphicCache) * sizeof(void*)), cache);
    notFound.append(jit.branchTestPtr(CCallHelpers::Zero, cache));
    CCallHelpers::JumpList notValue = jit.loadMegamorphicProperty(CCallHelpers::MegamorphicCacheLocation(cache), R0, uid, nullptr, result, T11, T12, T13);
    countAttempt(true);
    jit.move(result, R0);
    jit.ret();
    notValue.link(&jit);
    notFound.append(jit.loadMegamorphicGetterSetter(CCallHelpers::MegamorphicCacheLocation(cache), R0, uid, nullptr, result, T11, T12, T13));
    countAttempt(false);
    jit.move(result, T12);
    callGetter(jit, notFound);
}

static void generateGetByIdWith(CCallHelpers&, Entry, bool isCalledByThunk = false);
static void callLookUp(CCallHelpers&, Entry, GPRReg base, GPRReg name = InvalidGPRReg);

static void callLookUpAtSite(CCallHelpers& jit, Entry operation, GPRReg base, GPRReg name, GPRReg third)
{
    ASSERT(noOverlap(name, third, R0, A1) && third != A0 && third != A2 && name != A0);
    jit.emitFunctionPrologue();
#if CPU(ARM64)
    jit.pushPair(R0, A1);
#else
    jit.push(R0);
    jit.push(A1);
#endif
    jit.move(name, A1);
    jit.move(third, A2);
    jit.move(R0, A0);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T11);
    jit.loadPtr(Address(T11, static_cast<unsigned>(operation) * sizeof(void*)), T11);
    jit.call(T11, OperationPtrTag);
#if CPU(ARM64)
    jit.popPair(base, A1);
#else
    jit.pop(A1);
    jit.pop(base);
#endif
    jit.emitFunctionEpilogue();
}

static void readExport(CCallHelpers& jit, GPRReg nameSpace)
{
    checkStackBeforeCallingAccessor(jit);
    jit.zeroExtend32ToWord(R0, T13);
    jit.loadPtr(Address(nameSpace, Typed::ModuleNamespace::offsetOfRead()), calleeGPR);
    jit.add64(numberTag, T13, argumentGPR(0));
    jit.move(CCallHelpers::TrustedImm64(JSValue::ValueUndefined), thisGPR);
    jit.move(TrustedImm32(1), countGPR);
    jit.jump().linkTo(variadicCallCellLabel(), &jit);
}

static void askMegamorphicCache(CCallHelpers& jit, GPRReg uid, CCallHelpers::JumpList& hasPair, CCallHelpers::JumpList& hasNoPair)
{
#if CPU(X86_64)
    constexpr GPRReg cache = T10;
    constexpr GPRReg result = T9;
#else
    constexpr GPRReg cache = GPRInfo::argumentGPR7;
    constexpr GPRReg result = GPRInfo::argumentGPR6;
#endif
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), cache);
    jit.loadPtr(Address(cache, static_cast<unsigned>(Entry::MegamorphicCache) * sizeof(void*)), cache);
    hasNoPair.append(jit.branchTestPtr(CCallHelpers::Zero, cache));
    hasNoPair.append(jit.loadMegamorphicProperty(CCallHelpers::MegamorphicCacheLocation(cache), R0, uid, nullptr, result, T11, T12, T13));
    hasPair.append(jit.jump());
}
struct GetByIdContinuations {
    CCallHelpers::Label isIndirect;
    CCallHelpers::Label hasDifferentStructure;
    CCallHelpers::Label hasDifferentStructureAndIsIndirect;
    CCallHelpers::Label isNotNamed;
    CCallHelpers::Label miss;
};
static GetByIdContinuations& getByIdContinuations(Entry operation)
{
    static NeverDestroyed<std::array<GetByIdContinuations, 2>> continuations;
    return continuations.get()[operation == Entry::operationAOTGetByIdWellKnown];
}

static void readByName(CCallHelpers& jit, GPRReg base, CCallHelpers::JumpList& isIndirect, CCallHelpers::JumpList& isNotNamed)
{
    ASSERT(base != T9 && base != T11 && base != T12 && base != T13 && base != T14);
    isIndirect.append(branchIfSlotHas<Slot::isIndirect>(jit, T11));
    jit.urshift64(T11, TrustedImm32(32 + Slot::nameIDShift), T13);
#if CPU(X86_64)
    isNotNamed.append(Jump(jit.m_assembler.jCC(X86Assembler::ConditionE)));
#else
    isNotNamed.append(jit.branchTest64(CCallHelpers::Zero, T13));
#endif
    loadDirectLocation(jit, A1, T11, T14);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfFieldIDAtDirectLocationBase()), T9);
    jit.add64(T12, T9);
    jit.load16(CCallHelpers::BaseIndex(T9, T14, CCallHelpers::TimesTwo), T9);
    isNotNamed.append(jit.branch32(CCallHelpers::NotEqual, T9, T13));
    jit.load64(CCallHelpers::BaseIndex(base, T14, CCallHelpers::TimesEight), R0);
    jit.ret();
}

static void findPlaceByNameAndRow(CCallHelpers& jit, GPRReg name, GPRReg row, GPRReg displacement, GPRReg place, CCallHelpers::JumpList& isOfNoRow, CCallHelpers::JumpList& lacksName)
{
    ASSERT(noOverlap(R0, name, row, displacement, place));
    jit.load32(Address(R0, JSCell::structureIDOffset()), row);
    structureWithID(jit, row);
    jit.load16(Address(row, Structure::offsetOfNumberOfRowPlusOne()), row);
    isOfNoRow.append(jit.branchTest32(CCallHelpers::Zero, row));
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfDisplacementsOfNames()), place);
    jit.load32(CCallHelpers::BaseIndex(place, name, CCallHelpers::TimesFour), displacement);
    Jump isLiteral = jit.branch32(CCallHelpers::Below, row, Address(instanceGPR, Instance::offsetOfFirstRowOfThisPlusOne()));
    jit.or32(TrustedImm32(Typed::Data::nameIsOnObjectPrototype), displacement);
    isLiteral.link(&jit);
    jit.and32(TrustedImm32(~Typed::Data::nameIsOnObjectPrototype), displacement, place);
    jit.add32(row, place);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfPlacesByNameAndRow()), row);
    jit.load32(CCallHelpers::BaseIndex(row, place, CCallHelpers::TimesFour), place);
    jit.urshift32(place, TrustedImm32(8), row);
    lacksName.append(jit.branch32(CCallHelpers::NotEqual, row, name));
    jit.and32(TrustedImm32(0xff), place);
}

static void readByNameAndRow(CCallHelpers& jit, GPRReg name, GPRReg row, GPRReg displacement, GPRReg place, CCallHelpers::JumpList& isOfNoRow)
{
    CCallHelpers::JumpList lacksName;
    findPlaceByNameAndRow(jit, name, row, displacement, place, isOfNoRow, lacksName);
    jit.load64(CCallHelpers::BaseIndex(R0, place, CCallHelpers::TimesEight, JSObject::offsetOfInlineStorage()), place);
    isOfNoRow.append(jit.branchTest64(CCallHelpers::Zero, place));
    jit.move(place, R0);
    jit.ret();
    lacksName.link(&jit);
    isOfNoRow.append(jit.branchTest32(CCallHelpers::NonZero, displacement, TrustedImm32(Typed::Data::nameIsOnObjectPrototype)));
    jit.move(CCallHelpers::TrustedImm64(JSValue::ValueUndefined), R0);
    jit.ret();
}

static void generateTryGetByNameAndRow(CCallHelpers& jit)
{
    CCallHelpers::JumpList cannot;
    cannot.append(jit.branchIfNotCell(R0));
    cannot.append(jit.branchIfNotObject(R0));
    readByNameAndRow(jit, A1, T11, T13, T10, cannot);
    cannot.link(&jit);
    jit.move(TrustedImm32(0), R0);
    jit.ret();
}

static CCallHelpers::Label returnAfterStoring(CCallHelpers&, GPRReg value);

static void generateTryPutByNameAndRow(CCallHelpers& jit)
{
    CCallHelpers::JumpList cannot;
    cannot.append(jit.branchIfNotCell(R0));
    cannot.append(jit.branchIfNotObject(R0));
    findPlaceByNameAndRow(jit, A2, T11, T13, T10, cannot, cannot);
    jit.store64(A1, CCallHelpers::BaseIndex(R0, T10, CCallHelpers::TimesEight, JSObject::offsetOfInlineStorage()));
    returnAfterStoring(jit, A1);
    cannot.link(&jit);
    jit.move(TrustedImm32(0), R0);
    jit.ret();
}

static void generateGetById(CCallHelpers& jit) { generateGetByIdWith(jit, Entry::operationAOTGetById); }

// R0: an object. T12: the number of a name. See Instance::NamedField. Where the table has the pair, `access` is what the entry says and control goes on; T10, T11 and T13 are used. T12 is kept where it has not.
static void findNamedField(CCallHelpers& jit, ptrdiff_t offsetOfTable, GPRReg access, CCallHelpers::JumpList& isNotThere, bool isKnownToBeObjectType = false)
{
    ASSERT(access == T12 || access == T13);
    if (!isKnownToBeObjectType)
        isNotThere.append(jit.branch8(CCallHelpers::NotEqual, Address(R0, JSCell::typeInfoTypeOffset()), TrustedImm32(ObjectType)));
    jit.load32(Address(R0, Typed::offsetOfShapeWord), T11);
    jit.lshift32(T11, TrustedImm32(Instance::namedFieldShapeShift), T13);
    jit.add32(T12, T13);
    jit.and32(TrustedImm32(Instance::numberOfNamedFields - 1), T13);
    jit.lshiftPtr(TrustedImm32(4), T13);
    jit.loadPtr(Address(instanceGPR, offsetOfTable), T10);
    jit.addPtr(T10, T13);
    jit.lshift64(T12, TrustedImm32(32), T10);
    jit.or64(T10, T11);
    isNotThere.append(jit.branch64(CCallHelpers::NotEqual, Address(T13, OBJECT_OFFSETOF(Instance::NamedField, shapeWordAndName)), T11));
    jit.load64(Address(T13, OBJECT_OFFSETOF(Instance::NamedField, access)), access);
}

// R0: an object of a compiled layout. T12: Slot::pointer of an entry with Slot::compiledFieldFlags. R0 is kept until the value is there.
static void readCompiledField(CCallHelpers& jit, CCallHelpers::JumpList& rare, CCallHelpers::JumpList* isNoField = nullptr)
{
    using FieldRead = Slot::FieldRead;
    constexpr GPRReg address = T11;
    constexpr GPRReg value = T13;
    auto hasBit = [&](unsigned bit) {
#if CPU(X86_64)
        return jit.branchTestBit64(CCallHelpers::NonZero, T12, TrustedImm32(Slot::fieldReadShift + bit));
#else
        return jit.branchTest64(CCallHelpers::NonZero, T12, CCallHelpers::TrustedImm64(1ll << (Slot::fieldReadShift + bit)));
#endif
    };
    auto returnValue = [&] {
        jit.move(value, R0);
        jit.ret();
    };
    auto returnInt32 = [&] {
#if CPU(ARM64)
        jit.addZeroExtend64(numberTag, value, R0);
#else
        jit.zeroExtend32ToWord(value, value);
        jit.add64(numberTag, value, R0);
#endif
        jit.ret();
    };
    auto returnDoubleBits = [&] {
#if CPU(ARM64)
        jit.sub64(value, numberTag, R0);
        jit.ret();
#else
        jit.sub64(numberTag, value);
        returnValue();
#endif
    };
    CCallHelpers::JumpList isAbsent;
    CCallHelpers::JumpList isUndefined;
    CCallHelpers::JumpList isNull;
    static_assert(Slot::fieldIsInExtension == 1ull << (Slot::fieldReadShift + 4));
    Jump isInExtension = hasBit(4);
#if CPU(ARM64)
    jit.addZeroExtend64(R0, T12, address);
#else
    jit.zeroExtend32ToWord(T12, address);
    jit.addPtr(R0, address);
#endif
    CCallHelpers::Label hasAddress = jit.label();
    Jump isAmongUpperFour = hasBit(2);
    {
        Jump isAmongUpperTwo = hasBit(1);
        Jump isCell32 = hasBit(0);
        static_assert(!static_cast<unsigned>(FieldRead::Boxed));
        jit.load64(Address(address), value);
        isAbsent.append(jit.branchTest64(CCallHelpers::Zero, value));
        returnValue();

        static_assert(static_cast<unsigned>(FieldRead::Cell32) == 1);
        isCell32.link(&jit);
        jit.load32(Address(address), value);
        Jump isNoCell = jit.branch32(CCallHelpers::BelowOrEqual, value, TrustedImm32(Typed::undefinedCell32));
#if CPU(ARM64)
        decodeCell32(jit, value, R0);
        jit.ret();
#else
        jit.lshift64(TrustedImm32(Typed::cell32Shift), value);
        jit.add64(Address(instanceGPR, Instance::offsetOfCellCageBase()), value);
        returnValue();
#endif
        isNoCell.link(&jit);
        static_assert(!Typed::absentCell32);
        isAbsent.append(jit.branchTest32(CCallHelpers::Zero, value));
        returnValue();

        isAmongUpperTwo.link(&jit);
        jit.load32(Address(address), value);
        Jump isInt32 = hasBit(0);
        static_assert(static_cast<unsigned>(FieldRead::Int32OrElsewhere) == 2 && static_cast<unsigned>(FieldRead::Int32) == 3);
#if CPU(ARM64)
        static_assert(Typed::leastInt32InPlace == -(1 << 30));
        jit.m_assembler.bic<32, ARM64Assembler::S>(ARM64Registers::zr, value, value, ARM64Assembler::LSL, 1);
        Jump isNoNumberInPlace = jit.branchOnFlags(CCallHelpers::LessThan);
#else
        Jump isNoNumberInPlace = jit.branch32(CCallHelpers::LessThan, value, TrustedImm32(Typed::leastInt32InPlace));
#endif
        isInt32.link(&jit);
        returnInt32();
        isNoNumberInPlace.link(&jit);
        isAbsent.append(jit.branch32(CCallHelpers::Equal, value, TrustedImm32(Typed::absentInt32)));
        isUndefined.append(jit.branch32(CCallHelpers::Equal, value, TrustedImm32(Typed::undefinedInt32)));
        isNull.append(jit.branch32(CCallHelpers::Equal, value, TrustedImm32(Typed::nullInt32)));
        jit.and32(TrustedImm32(INT32_MAX), value);
        jit.lshift64(TrustedImm32(Typed::numberBoxShift), value);
        jit.add64(Address(instanceGPR, Instance::offsetOfNumberBoxBase()), value);
        jit.loadDouble(Address(value), FPRInfo::fpRegT0);
        rare.append(jit.branchIfNaN(FPRInfo::fpRegT0));
        CCallHelpers::JumpList isNoInt32;
        branchIfNoExactInt32(jit, FPRInfo::fpRegT0, value, isNoInt32, FPRInfo::fpRegT1);
        returnInt32();
        isNoInt32.link(&jit);
        jit.moveDoubleTo64(FPRInfo::fpRegT0, value);
        returnDoubleBits();
    }
    isAmongUpperFour.link(&jit);
    Jump isAmongLastTwo = hasBit(1);
    {
        Jump isBoolean = hasBit(0);
        static_assert(static_cast<unsigned>(FieldRead::Double) == 4);
        jit.loadDouble(Address(address), FPRInfo::fpRegT0);
        Jump isNaN = jit.branchIfNaN(FPRInfo::fpRegT0);
        CCallHelpers::JumpList isNoInt32;
        branchIfNoExactInt32(jit, FPRInfo::fpRegT0, value, isNoInt32, FPRInfo::fpRegT1);
        returnInt32();
        isNoInt32.link(&jit);
        jit.moveDoubleTo64(FPRInfo::fpRegT0, value);
        returnDoubleBits();
        isNaN.link(&jit);
        jit.moveDoubleTo64(FPRInfo::fpRegT0, value);
        isAbsent.append(jit.branch64(CCallHelpers::Equal, value, CCallHelpers::TrustedImm64(static_cast<int64_t>(Typed::absentDouble))));
        isUndefined.append(jit.branch64(CCallHelpers::Equal, value, CCallHelpers::TrustedImm64(static_cast<int64_t>(Typed::undefinedDouble))));
        isNull.append(jit.branch64(CCallHelpers::Equal, value, CCallHelpers::TrustedImm64(static_cast<int64_t>(Typed::nullDouble))));
        rare.append(jit.jump());

        static_assert(static_cast<unsigned>(FieldRead::Boolean) == 5);
        isBoolean.link(&jit);
        jit.load8(Address(address), value);
#if CPU(ARM64)
        jit.or32(TrustedImm32(JSValue::ValueFalse), value, R0);
        jit.ret();
#else
        jit.or32(TrustedImm32(JSValue::ValueFalse), value);
        returnValue();
#endif
    }
    isAmongLastTwo.link(&jit);
    Jump isInt64 = hasBit(0);
    static_assert(static_cast<unsigned>(FieldRead::OptionalBoolean) == 6);
    jit.load8(Address(address), value);
    isAbsent.append(jit.branchTest32(CCallHelpers::Zero, value));
    returnValue();

    static_assert(static_cast<unsigned>(FieldRead::Int64) == 7);
    isInt64.link(&jit);
    jit.load64(Address(address), value);
    jit.signExtend32ToPtr(value, address);
    Jump isBeyondInt32 = jit.branch64(CCallHelpers::NotEqual, value, address);
    returnInt32();
    isBeyondInt32.link(&jit);
    isAbsent.append(jit.branch64(CCallHelpers::Equal, value, CCallHelpers::TrustedImm64(Typed::absentInteger)));
    isUndefined.append(jit.branch64(CCallHelpers::Equal, value, CCallHelpers::TrustedImm64(Typed::undefinedInteger)));
    isNull.append(jit.branch64(CCallHelpers::Equal, value, CCallHelpers::TrustedImm64(Typed::nullInteger)));
    rare.append(jit.jump());

    isInExtension.link(&jit);
    static_assert(static_cast<unsigned>(FieldRead::Uint8) == 0x30 && static_cast<unsigned>(FieldRead::Uint32) == 0x34);
    Jump isNarrow = hasBit(5);
    jit.load32(Address(R0, Typed::offsetOfExtension), address);
    isAbsent.append(jit.branchTest32(CCallHelpers::Zero, address));
    decodeCell32(jit, address, address);
    jit.load32(Address(address, Typed::offsetOfExtensionSize), value);
    isAbsent.append(jit.branch32(CCallHelpers::BelowOrEqual, value, T12));
    jit.zeroExtend32ToWord(T12, value);
    jit.addPtr(value, address);
    jit.jump().linkTo(hasAddress, &jit);

    isAbsent.link(&jit);
    rare.append(jit.branchTest64(CCallHelpers::Zero, T12, CCallHelpers::TrustedImm64(static_cast<int64_t>(Slot::absentFieldIsUndefined))));
    isUndefined.link(&jit);
    jit.move(CCallHelpers::TrustedImm64(JSValue::ValueUndefined), R0);
    jit.ret();
    isNull.link(&jit);
    jit.move(CCallHelpers::TrustedImm64(JSValue::ValueNull), R0);
    jit.ret();

    isNarrow.link(&jit);
    Jump isUint32 = hasBit(2);
    jit.zeroExtend32ToWord(T12, address);
    jit.addPtr(R0, address);
    Jump hasTwoBytes = hasBit(1);
    Jump isInt8 = hasBit(0);
    jit.load8(Address(address), value);
    returnInt32();
    isInt8.link(&jit);
    jit.load8SignedExtendTo32(Address(address), value);
    returnInt32();
    hasTwoBytes.link(&jit);
    Jump isInt16 = hasBit(0);
    jit.load16(Address(address), value);
    returnInt32();
    isInt16.link(&jit);
    jit.load16SignedExtendTo32(Address(address), value);
    returnInt32();
    isUint32.link(&jit);
    static_assert(static_cast<unsigned>(FieldRead::NoField) == 0x37);
    (isNoField ? *isNoField : rare).append(hasBit(1));
    jit.zeroExtend32ToWord(T12, address);
    jit.addPtr(R0, address);
    jit.load32(Address(address), value);
    Jump isNoInt32 = jit.branch32(CCallHelpers::LessThan, value, TrustedImm32(0));
    returnInt32();
    isNoInt32.link(&jit);
    jit.convertInt64ToDouble(value, FPRInfo::fpRegT0);
    jit.moveDoubleTo64(FPRInfo::fpRegT0, value);
    returnDoubleBits();
}

static void callKnownCustomGetter(CCallHelpers& jit, CCallHelpers::JumpList& isNotKnown, bool callerIsThunk)
{
    using Known = Instance::CustomGetter;
    constexpr GPRReg known = T13;
    static_assert(noOverlap(known, T11, R0, A1, A2) && noOverlap(known, T11, A0, A1, A2));
    static_assert(static_cast<unsigned>(Slot::FieldRead::NoField) == (static_cast<unsigned>(Slot::FieldRead::KnownCustomGetter) | 1));
#if CPU(X86_64)
    isNotKnown.append(jit.branchTestBit64(CCallHelpers::NonZero, T12, TrustedImm32(Slot::fieldReadShift)));
#else
    isNotKnown.append(jit.branchTest64(CCallHelpers::NonZero, T12, CCallHelpers::TrustedImm64(1ll << Slot::fieldReadShift)));
#endif
    jit.urshift64(T12, TrustedImm32(Slot::customGetterPlaceShift), known);
    jit.addPtr(Address(instanceGPR, Instance::offsetOfCustomGetters()), known);
    isNotKnown.append(jit.branch32(CCallHelpers::NotEqual, Address(known, OBJECT_OFFSETOF(Known, serial)), T12));
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T11);
    jit.loadPtr(Address(T11, static_cast<unsigned>(Entry::MegamorphicCache) * sizeof(void*)), T11);
    jit.load16(Address(T11, MegamorphicCache::offsetOfEpoch()), T11);
    jit.load16(Address(known, OBJECT_OFFSETOF(Known, epoch)), T12);
    isNotKnown.append(jit.branch32(CCallHelpers::NotEqual, T11, T12));

    if (callerIsThunk) {
#if CPU(ARM64)
        jit.pushToSave(CCallHelpers::linkRegister);
#else
        jit.subPtr(TrustedImm32(sizeof(CPURegister)), CCallHelpers::stackPointerRegister);
#endif
    } else
        jit.emitFunctionPrologue();
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfVM()), T11);
    jit.storePtr(GPRInfo::callFrameRegister, Address(T11, VM::offsetOfTopCallFrame()));
    jit.loadPtr(Address(known, OBJECT_OFFSETOF(Known, passedForReceiver)), A1);
    jit.moveConditionallyTest64(CCallHelpers::Zero, A1, A1, R0, A1);
    jit.loadPtr(Address(known, OBJECT_OFFSETOF(Known, uid)), A2);
    jit.loadPtr(Address(known, OBJECT_OFFSETOF(Known, getter)), T11);
    jit.loadPtr(Address(known, OBJECT_OFFSETOF(Known, globalObject)), A0);
    jit.call(T11, CustomAccessorPtrTag);
    if (callerIsThunk) {
#if CPU(ARM64)
        jit.popToRestore(CCallHelpers::linkRegister);
#else
        jit.addPtr(TrustedImm32(sizeof(CPURegister)), CCallHelpers::stackPointerRegister);
#endif
    } else
        jit.emitFunctionEpilogue();
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfVM()), T11);
    Jump exception = jit.branchTestPtr(CCallHelpers::NonZero, Address(T11, VM::exceptionOffset()));
    jit.move(GPRInfo::returnValueGPR, R0);
    jit.ret();
    exception.link(&jit);
    if (callerIsThunk)
        jit.emitFunctionEpilogue();
    loadInstance(jit, T9);
    jumpToEntry(jit, T9, Entry::HandleException);
}

static void generateGetByIdWith(CCallHelpers& jit, Entry operation, bool isCalledByThunk)
{
    CCallHelpers::JumpList miss;
    CCallHelpers::JumpList isNoField;
    miss.append(jit.branchIfNotCell(R0));
    jit.load64(slotWord(A1, 0), T11);
    Jump hasDifferentStructure = branchIfStructureDiffers(jit, R0, T11);
    Jump isIndirect = branchIfSlotHas<Slot::isIndirect>(jit, T11);
    loadDirectLocation(jit, A1, T11, T11);
    jit.load64(CCallHelpers::BaseIndex(R0, T11, CCallHelpers::TimesEight), R0);
    jit.ret();

    isIndirect.link(&jit);
    if (!isCalledByThunk)
        getByIdContinuations(operation).isIndirect = jit.label();
    jit.loadPtr(slotWord(A1, 1), T12);
    Jump isGetter = branchIfSlotHas<Slot::isGetter>(jit, T11);
    jit.moveConditionallyTest64(CCallHelpers::NonZero, T12, T12, T12, R0, T12);
    locateCachedProperty(jit, T12, T11, T13);
    jit.load64(CCallHelpers::BaseIndex(T13, T11, CCallHelpers::TimesEight), R0);
    jit.ret();

    isGetter.link(&jit);
    {
        Jump isField = branchIfSlotHas<Slot::pointerIsNotCell>(jit, T11);
        jit.moveConditionallyTest64(CCallHelpers::NonZero, T12, T12, T12, R0, T12);
        locateCachedProperty(jit, T12, T11, T13);
        jit.load64(CCallHelpers::BaseIndex(T13, T11, CCallHelpers::TimesEight), T12);
        callGetter(jit, miss);
        isField.link(&jit);
        readCompiledField(jit, miss, &isNoField);
    }

    hasDifferentStructure.link(&jit);
    if (!isCalledByThunk)
        getByIdContinuations(operation).hasDifferentStructure = jit.label();
#if CPU(X86_64)
    jit.load32(Address(R0, JSCell::structureIDOffset()), T12);
#endif
    CCallHelpers::JumpList missAndUpdateCache;
    if (operation == Entry::operationAOTGetById) {
        constexpr GPRReg several = T13;
#if CPU(X86_64)
        constexpr GPRReg entry = A2;
        constexpr GPRReg table = T10;
        constexpr GPRReg nameID = T9;
        constexpr GPRReg nameIDThere = T12;
#else
        constexpr GPRReg entry = T14;
        constexpr GPRReg table = GPRInfo::argumentGPR6;
        constexpr GPRReg nameID = A5;
        constexpr GPRReg nameIDThere = A4;
#endif
        {
            CCallHelpers::JumpList isAlsoIndirect;
            CCallHelpers::JumpList isNotNamed;
            readByName(jit, R0, isAlsoIndirect, isNotNamed);
            isNotNamed.link(&jit);
            if (!isCalledByThunk)
                getByIdContinuations(operation).isNotNamed = jit.label();
            miss.append(jit.branchTest64(CCallHelpers::Zero, T11, CCallHelpers::TrustedImm64(static_cast<int64_t>(~(static_cast<uint64_t>(Slot::attemptsMask) << 32)))));
            missAndUpdateCache.append(jit.jump());
            isAlsoIndirect.link(&jit);
            if (!isCalledByThunk)
                getByIdContinuations(operation).hasDifferentStructureAndIsIndirect = jit.label();
        }
        missAndUpdateCache.append(jit.branchTest32(CCallHelpers::NonZero, T11));
        // No structure, and indirect: of such slots only one that stands for several has this flag.
        static_assert(Slot::polymorphicFlags == (Slot::isIndirect | Slot::pointerIsNotCell));
        miss.append(branchUnlessSlotHas<Slot::pointerIsNotCell>(jit, T11));
        jit.loadPtr(slotWord(A1, 1), several);
        static_assert(!OBJECT_OFFSETOF(Slot, structureID));

        // The one slot that the structure can have there: PolymorphicSlots::indexFor().
        static_assert(sizeof(Slot) == 16 && PolymorphicSlots::structureShift >= 4);
        jit.and32(TrustedImm32((PolymorphicSlots::numberOfSlots - 1) << PolymorphicSlots::structureShift), T12, T11);
        jit.urshift32(TrustedImm32(PolymorphicSlots::structureShift - 4), T11);
#if CPU(X86_64)
        jit.x86Lea64(CCallHelpers::BaseIndex(several, T11, CCallHelpers::TimesOne, PolymorphicSlots::offsetOfSlots()), entry);
#else
        jit.addPtr(several, T11, entry);
        jit.addPtr(TrustedImm32(PolymorphicSlots::offsetOfSlots()), entry);
#endif
        jit.load64(slotWord(entry, 0), T11);
        Jump entryMatch = jit.branch32(CCallHelpers::Equal, T11, T12);

        // The property by its name's number, which the structure records for its first inline slots: for any number of structures.
        constexpr GPRReg byName = T11;
        constexpr GPRReg inlineSlot = A2;
        static_assert(!(PolymorphicSlots::inlineNameSlotsShift % 8));
        auto loadInlineNameSlot = [&](unsigned i, GPRReg result) {
#if CPU(X86_64)
            jit.load8(Address(several, PolymorphicSlots::offsetOfByName() + PolymorphicSlots::inlineNameSlotsShift / 8 + i), result);
#else
            extractBits(jit, byName, PolymorphicSlots::inlineNameSlotsShift + i * 8, 8, result);
#endif
        };
        jit.load64(Address(several, PolymorphicSlots::offsetOfByName()), byName);
        jit.zeroExtend16To32(byName, nameID);
        Jump hasNoInlineNameSlots = jit.branch32(CCallHelpers::Equal, nameID, TrustedImm32(Structure::firstReservedPropertyNameID));
        jit.zeroExtend32ToWord(T12, table);
        structureWithID(jit, table);
#if CPU(X86_64)
        const int32_t offsetInTable = Structure::offsetOfFieldIDInSlot();
#else
        const int32_t offsetInTable = 0;
        jit.addPtr(TrustedImm32(Structure::offsetOfFieldIDInSlot()), table);
#endif
        loadInlineNameSlot(0, inlineSlot);
        jit.load16(CCallHelpers::BaseIndex(table, inlineSlot, CCallHelpers::TimesTwo, offsetInTable), nameIDThere);
        Jump isInFirstInlineNameSlot = jit.branch32(CCallHelpers::Equal, nameIDThere, nameID);
        for (unsigned i = 1; i < PolymorphicSlots::numberOfInlineSlotsByName; ++i) {
            loadInlineNameSlot(i, A3);
            jit.load16(CCallHelpers::BaseIndex(table, A3, CCallHelpers::TimesTwo, offsetInTable), nameIDThere);
            jit.moveConditionally32(CCallHelpers::Equal, nameIDThere, nameID, A3, inlineSlot, inlineSlot);
        }
        jit.load16(CCallHelpers::BaseIndex(table, inlineSlot, CCallHelpers::TimesTwo, offsetInTable), nameIDThere);
        Jump isNotInInlineNameSlots = jit.branch32(CCallHelpers::NotEqual, nameIDThere, nameID);
        isInFirstInlineNameSlot.link(&jit);
        jit.load64(CCallHelpers::BaseIndex(R0, inlineSlot, CCallHelpers::TimesEight, JSObject::offsetOfInlineStorage()), R0);
        jit.ret();
        isNotInInlineNameSlots.link(&jit);
        jit.urshift64(byName, TrustedImm32(PolymorphicSlots::nameTableFillValueShift), nameID);
        for (unsigned i = 0; i < PolymorphicSlots::numberOfInlineSlotsByName; ++i) {
            loadInlineNameSlot(i, A3);
            jit.load16(CCallHelpers::BaseIndex(table, A3, CCallHelpers::TimesTwo, offsetInTable), nameIDThere);
            missAndUpdateCache.append(jit.branch32(CCallHelpers::Equal, nameIDThere, nameID));
        }
        hasNoInlineNameSlots.link(&jit);
        jit.load32(Address(several, PolymorphicSlots::offsetOfMisses()), T12);
        jit.add32(TrustedImm32(1), T12);
        jit.store32(T12, Address(several, PolymorphicSlots::offsetOfMisses()));
        Jump hasHadItsFirstLessons = jit.branchTest32(CCallHelpers::Zero, Address(several, PolymorphicSlots::offsetOfRemainingBulkLearnAttempts()));
        Jump isEarlyLesson = jit.branchTest32(CCallHelpers::Zero, T12, TrustedImm32(PolymorphicSlots::missesPerEarlyLesson - 1));
        Jump isNoLesson = jit.jump();
        hasHadItsFirstLessons.link(&jit);
        missAndUpdateCache.append(jit.branchTest32(CCallHelpers::Zero, T12, TrustedImm32(PolymorphicSlots::missesPerLearningAttempt - 1)));
        isNoLesson.link(&jit);
        static_assert(FinalObjectType > ObjectType);
        jit.load8(Address(R0, JSCell::typeInfoTypeOffset()), T11);
        Jump isObjectTypeOrLower = jit.branch32(CCallHelpers::BelowOrEqual, T11, TrustedImm32(ObjectType));
        {
            CCallHelpers::JumpList isOfNoRow;
            jit.load32(Address(several, PolymorphicSlots::offsetOfIdentifierIndex()), T12);
            jit.and32(TrustedImm32((1u << Site::identifierBits) - 1), T12);
            readByNameAndRow(jit, T12, T11, A3, A2, isOfNoRow);
            isOfNoRow.link(&jit);
        }
        CCallHelpers::Label probeMegamorphicCache = jit.label();
        jit.loadPtr(Address(several, PolymorphicSlots::offsetOfName()), A2);
        CCallHelpers::JumpList isInNoCache;
        getFromMegamorphicCache(jit, A2, isInNoCache);

        isEarlyLesson.link(&jit);
        missAndUpdateCache.append(jit.branchTest8(CCallHelpers::Zero, Address(several, PolymorphicSlots::offsetOfOwesItsLessons())));
        jit.loadPtr(Address(several, PolymorphicSlots::offsetOfName()), A2);
        askMegamorphicCache(jit, A2, missAndUpdateCache, isInNoCache);

        isInNoCache.link(&jit);
        jit.loadPtr(slotWord(A1, 1), several);
        missAndUpdateCache.append(jit.branchTest8(CCallHelpers::Zero, Address(several, PolymorphicSlots::offsetOfOwesItsLessons())));
        jit.load8(Address(R0, JSCell::typeInfoTypeOffset()), T11);
        {
            Jump isFinal = jit.branch32(CCallHelpers::Equal, T11, TrustedImm32(FinalObjectType));
            Jump isArray = jit.branch32(CCallHelpers::Equal, T11, TrustedImm32(ArrayType));
            Jump isFunction = jit.branch32(CCallHelpers::Equal, T11, TrustedImm32(JSFunctionType));
            missAndUpdateCache.append(jit.branch32(CCallHelpers::NotEqual, T11, TrustedImm32(ObjectType)));
            jit.loadPtr(Address(instanceGPR, Instance::offsetOfTyped()), T11);
            missAndUpdateCache.append(jit.branchTestPtr(CCallHelpers::Zero, T11));
            jit.loadPtr(Address(T11, Typed::State::offsetOfModuleNamespaceStructure()), T11);
            jit.load32(Address(R0, JSCell::structureIDOffset()), T12);
            structureWithID(jit, T12);
            missAndUpdateCache.append(jit.branchPtr(CCallHelpers::NotEqual, T11, T12));
            jit.loadPtr(Address(several, PolymorphicSlots::offsetOfName()), A2);
            callLookUpAtSite(jit, Entry::operationAOTGetPlainlyAtSite, T12, A2, several);
            Jump isNoExport = jit.branchTest64(CCallHelpers::Zero, R0);
            readExport(jit, T12);
            isNoExport.link(&jit);
            jit.move(T12, R0);
            missAndUpdateCache.append(jit.jump());
            isFinal.link(&jit);
            isArray.link(&jit);
            isFunction.link(&jit);
        }
        jit.loadPtr(Address(several, PolymorphicSlots::offsetOfName()), A2);
        callLookUpAtSite(jit, Entry::operationAOTGetPlainlyAtSite, T12, A2, several);
        Jump cannotLookUpAtSite = jit.branchTest64(CCallHelpers::Zero, R0);
        jit.ret();
        cannotLookUpAtSite.link(&jit);
        jit.move(T12, R0);
        missAndUpdateCache.append(jit.jump());

        isObjectTypeOrLower.link(&jit);
        missAndUpdateCache.append(jit.branch32(CCallHelpers::NotEqual, T11, TrustedImm32(ObjectType)));
        CCallHelpers::JumpList isNoKnownField;
        jit.load32(Address(several, PolymorphicSlots::offsetOfIdentifierIndex()), T12);
        jit.branchTest32(CCallHelpers::NonZero, T12, TrustedImm32(PolymorphicSlots::hasNotReadCompiledField)).linkTo(probeMegamorphicCache, &jit);
        findNamedField(jit, Instance::offsetOfNamedFieldReads(), T12, isNoKnownField, true);
        Jump isKnownField = jit.jump();
        isNoKnownField.link(&jit);
        jit.loadPtr(slotWord(A1, 1), several);
        jit.jump().linkTo(probeMegamorphicCache, &jit);

        entryMatch.link(&jit);
        Jump isIndirectLocation = branchIfSlotHas<Slot::isIndirect>(jit, T11);
        loadDirectLocation(jit, entry, T11, T11);
        jit.load64(CCallHelpers::BaseIndex(R0, T11, CCallHelpers::TimesEight), R0);
        jit.ret();
        isIndirectLocation.link(&jit);
        jit.loadPtr(slotWord(entry, 1), T12);
        jit.moveConditionallyTest64(CCallHelpers::NonZero, T12, T12, T12, R0, T12);
        Jump isGetterThere = branchIfSlotHas<Slot::isGetter>(jit, T11);
        locateCachedProperty(jit, T12, T11, T13);
        jit.load64(CCallHelpers::BaseIndex(T13, T11, CCallHelpers::TimesEight), R0);
        jit.ret();
        isGetterThere.link(&jit);
        CCallHelpers::JumpList cannotCallGetter;
        Jump isFieldThere = branchIfSlotHas<Slot::pointerIsNotCell>(jit, T11);
        locateCachedProperty(jit, T12, T11, T13);
        jit.load64(CCallHelpers::BaseIndex(T13, T11, CCallHelpers::TimesEight), T12);
        callGetter(jit, cannotCallGetter);
        isFieldThere.link(&jit);
        isKnownField.link(&jit);
        CCallHelpers::JumpList isNoFieldThere;
        readCompiledField(jit, cannotCallGetter, &isNoFieldThere);
        isNoFieldThere.link(&jit);
        static_assert(static_cast<unsigned>(Slot::FieldRead::NoField) == (static_cast<unsigned>(Slot::FieldRead::KnownCustomGetter) | 1));
#if CPU(X86_64)
        isNoField.append(jit.branchTestBit64(CCallHelpers::Zero, T12, TrustedImm32(Slot::fieldReadShift)));
#else
        isNoField.append(jit.branchTest64(CCallHelpers::Zero, T12, CCallHelpers::TrustedImm64(1ll << Slot::fieldReadShift)));
#endif
        static_assert(Slot::isAbsentWhileCountStands == ((Slot::knownCustomGetter & ~1ull) | 4) && Slot::isLookedUp == ((Slot::knownCustomGetter & ~1ull) | 2));
        Jump isAbsentWhileCountStands = jit.branchTest32(CCallHelpers::NonZero, T12, TrustedImm32(4));
        cannotCallGetter.append(jit.branchTest32(CCallHelpers::Zero, T12, TrustedImm32(2)));
        jit.loadPtr(slotWord(A1, 1), several);
        jit.loadPtr(Address(several, PolymorphicSlots::offsetOfName()), A2);
        callLookUp(jit, Entry::operationAOTGetPlainly, T12, A2);
        Jump cannotLookUp = jit.branchTest64(CCallHelpers::Zero, R0);
        jit.ret();
        cannotLookUp.link(&jit);
        jit.move(T12, R0);
        cannotCallGetter.append(jit.jump());
        isAbsentWhileCountStands.link(&jit);
        jit.load32(Address(R0, JSCell::structureIDOffset()), T11);
        structureWithID(jit, T11);
        jit.loadPtr(Address(T11, Structure::propertyTableUnsafeOffset()), T11);
        cannotCallGetter.append(jit.branchTestPtr(CCallHelpers::Zero, T11));
        jit.load32(Address(T11, PropertyTable::offsetOfKeyCount()), T11);
        jit.urshift32(TrustedImm32(Slot::countShift), T12);
        cannotCallGetter.append(jit.branch32(CCallHelpers::NotEqual, T11, T12));
        jit.move(CCallHelpers::TrustedImm64(JSValue::ValueUndefined), R0);
        jit.ret();
        cannotCallGetter.link(&jit);
        missAndUpdateCache.append(jit.jump());
    }

    miss.link(&jit);
    if (!isCalledByThunk)
        getByIdContinuations(operation).miss = jit.label();
    CCallHelpers::JumpList notFound;
    constexpr bool keepsCallerIndex = T15 != T9;
    if (operation == Entry::operationAOTGetById) {
        loadCallerIndex(jit, 0, isCalledByThunk, false);
        loadDataForSlot(jit, A1, T10);
        countSlotMiss(jit, T10);

        notFound.append(jit.branchIfNotCell(R0));
        notFound.append(jit.branchIfNotObject(R0));
        jit.load32(Address(A1, OBJECT_OFFSETOF(Slot, offset)), T11);
        jit.and32(TrustedImm32(Slot::attemptsMask), T11);
        constexpr GPRReg dataOfUntriedSlot = T14;
        jit.move(CCallHelpers::TrustedImmPtr(nullptr), dataOfUntriedSlot);
        // T10: the Data, since the miss was counted.
        Jump hasGivenUp = jit.branch32(CCallHelpers::Equal, T11, TrustedImm32(Slot::attemptsMask));
        Jump isTaken = jit.branchTest32(CCallHelpers::NonZero, Address(A1, OBJECT_OFFSETOF(Slot, structureID)));
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfSharedData()), T12);
        Jump isUnowned = jit.branchPtr(CCallHelpers::Equal, T10, T12);
        notFound.append(jit.branchTest32(CCallHelpers::NonZero, T11));
        jit.move(T10, dataOfUntriedSlot);
        Jump isUntried = jit.jump();
        hasGivenUp.link(&jit);
        isTaken.link(&jit);
        isUnowned.link(&jit);
        isUntried.link(&jit);
        CCallHelpers::BaseIndex siteOfSlot = siteOfSlotIn(jit, T10, A1, T11, T13);
        loadInfo(jit, T10);
        loadSites(jit, T10, T11);
        jit.load32(siteOfSlot, T12);
        jit.and32(TrustedImm32((1u << Site::identifierBits) - 1), T12);
        {
            CCallHelpers::JumpList isNoKnownField;
            // Only for a slot that cannot learn. One that can goes on to the operation, which fills it.
            isNoKnownField.append(jit.branchTestPtr(CCallHelpers::NonZero, dataOfUntriedSlot));
            findNamedField(jit, Instance::offsetOfNamedFieldReads(), T12, isNoKnownField);
            readCompiledField(jit, notFound);
            isNoKnownField.link(&jit);
        }
        {
            CCallHelpers::JumpList isOfNoRow;
            isOfNoRow.append(jit.branchTestPtr(CCallHelpers::NonZero, dataOfUntriedSlot));
            readByNameAndRow(jit, T12, T11, T13, T10, isOfNoRow);
            isOfNoRow.link(&jit);
        }
        jit.move(T12, T10);
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfProgramIdentifierPlaces()), T11);
        jit.load16(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesTwo), T12);
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfProgramIdentifiers()), T11);
        jit.loadPtr(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesEight), A2);
        notFound.append(jit.branchTestPtr(CCallHelpers::Zero, A2));
        {
            static_assert(sizeof(Structure::SeenProperties) == sizeof(uint64_t));
            CCallHelpers::JumpList mayBeThere;
            mayBeThere.append(jit.branchTestPtr(CCallHelpers::NonZero, dataOfUntriedSlot));
            mayBeThere.append(jit.branch8(CCallHelpers::NotEqual, Address(R0, JSCell::typeInfoTypeOffset()), TrustedImm32(FinalObjectType)));
            mayBeThere.append(jit.branchTest8(CCallHelpers::NonZero, Address(R0, JSCell::indexingTypeAndMiscOffset()), TrustedImm32(IndexingShapeMask)));
            jit.load32(Address(R0, JSCell::structureIDOffset()), T11);
            structureWithID(jit, T11);
            jit.load64(Address(T11, Structure::seenPropertiesOffset()), T13);
            jit.and64(A2, T13);
            mayBeThere.append(jit.branch64(CCallHelpers::Equal, T13, A2));
            jit.load64(Address(T11, Structure::prototypeOffset()), T13);
            mayBeThere.append(jit.branch64(CCallHelpers::NotEqual, T13, Address(instanceGPR, Instance::offsetOfObjectPrototypeOfRealm())));
            jit.loadPtr(Address(instanceGPR, Instance::offsetOfDisplacementsOfNames()), T13);
            jit.load32(CCallHelpers::BaseIndex(T13, T10, CCallHelpers::TimesFour), T13);
            mayBeThere.append(jit.branchTest32(CCallHelpers::NonZero, T13, TrustedImm32(Typed::Data::nameIsOnObjectPrototype)));
            jit.move(CCallHelpers::TrustedImm64(JSValue::ValueUndefined), R0);
            jit.ret();
            mayBeThere.link(&jit);
        }
        CCallHelpers::JumpList isInNoCache;
        getFromMegamorphicCache(jit, A2, isInNoCache, dataOfUntriedSlot);
        isInNoCache.link(&jit);
        notFound.append(jit.branchTestPtr(CCallHelpers::NonZero, dataOfUntriedSlot));
        notFound.append(jit.branchTest32(CCallHelpers::NonZero, Address(A1, OBJECT_OFFSETOF(Slot, structureID))));
        jit.load8(Address(R0, JSCell::typeInfoTypeOffset()), T11);
        {
            Jump isFinal = jit.branch32(CCallHelpers::Equal, T11, TrustedImm32(FinalObjectType));
            Jump isArray = jit.branch32(CCallHelpers::Equal, T11, TrustedImm32(ArrayType));
            Jump isFunction = jit.branch32(CCallHelpers::Equal, T11, TrustedImm32(JSFunctionType));
            notFound.append(jit.branch32(CCallHelpers::NotEqual, T11, TrustedImm32(ObjectType)));
            jit.loadPtr(Address(instanceGPR, Instance::offsetOfTyped()), T11);
            notFound.append(jit.branchTestPtr(CCallHelpers::Zero, T11));
            jit.loadPtr(Address(T11, Typed::State::offsetOfModuleNamespaceStructure()), T11);
            jit.load32(Address(R0, JSCell::structureIDOffset()), T12);
            structureWithID(jit, T12);
            notFound.append(jit.branchPtr(CCallHelpers::NotEqual, T11, T12));
            callLookUpAtSite(jit, Entry::operationAOTGetPlainlyAndFill, T12, A2, instanceGPR);
            Jump isNoExport = jit.branchTest64(CCallHelpers::Zero, R0);
            readExport(jit, T12);
            isNoExport.link(&jit);
            jit.move(T12, R0);
            missAndUpdateCache.append(jit.jump());
            isFinal.link(&jit);
            isArray.link(&jit);
            isFunction.link(&jit);
        }
        callLookUpAtSite(jit, Entry::operationAOTGetPlainlyAndFill, T12, A2, instanceGPR);
        Jump cannotLookUp = jit.branchTest64(CCallHelpers::Zero, R0);
        jit.ret();
        cannotLookUp.link(&jit);
        jit.move(T12, R0);
        missAndUpdateCache.append(jit.jump());
        if (!keepsCallerIndex)
            notFound.link(&jit);
    }
    if (operation != Entry::operationAOTGetById)
        isNoField.link(&jit);
    missAndUpdateCache.link(&jit);
    CCallHelpers::Label goesToOperation = jit.label();
    missAtSite(jit, operation == Entry::operationAOTGetById ? Entry::RawGetById : operation, 1, Returns::Value, keepsCallerIndex ? &notFound : nullptr, isCalledByThunk);
    if (operation == Entry::operationAOTGetById) {
        CCallHelpers::JumpList isNotKnown;
        isNoField.link(&jit);
        callKnownCustomGetter(jit, isNotKnown, isCalledByThunk);
        isNotKnown.linkTo(goesToOperation, &jit);
    }
}

// R0: an object of a compiled layout. A1: the value. T13: Slot::pointer of an entry for a store to a field. T14: its upper half, see Slot::FieldWrite. R0, A1 and A2 are kept.
// `created`: the field is not there yet, and whatever its slot holds says nothing: the store is made and control goes on there, with T10 kept as well. See storeByTransition().
static void storeCompiledField(CCallHelpers& jit, CCallHelpers::JumpList& miss, CCallHelpers::JumpList* created = nullptr)
{
    using FieldWrite = Slot::FieldWrite;
    constexpr GPRReg address = T12;
    constexpr GPRReg scratch = T11;
    auto unlessAccepts = [&](uint32_t kind) {
        miss.append(jit.branchTest32(CCallHelpers::Zero, T14, TrustedImm32(kind)));
    };
    auto isValue = [&](int64_t encoded) {
        return jit.branch64(CCallHelpers::Equal, A1, CCallHelpers::TrustedImm64(encoded));
    };
    auto unlessTypeAcceptsObject = [&] {
        using Typed::RuntimeShapeTest;
        using Typed::RuntimeType;
        constexpr GPRReg type = T13;
        constexpr GPRReg other = T14;
        constexpr unsigned maxViews = 3;
        constexpr unsigned maxJSTypes = 2;
        CCallHelpers::JumpList isAccepted;
        CCallHelpers::JumpList hasNoMoreViews;
        CCallHelpers::JumpList hasNoMoreJSTypes;
        jit.urshift32(TrustedImm32(FieldWrite::offsetBits), type);
        jit.urshift32(TrustedImm32(FieldWrite::typeShift), other);
        jit.and32(TrustedImm32((1 << FieldWrite::typeBitsInUpperHalf) - 1), other);
        jit.lshift32(TrustedImm32(32 - FieldWrite::offsetBits), other);
        jit.or32(other, type);
        miss.append(jit.branchTest32(CCallHelpers::Zero, type));
        jit.lshift64(TrustedImm32(2), type);
        jit.addPtr(Address(instanceGPR, Instance::offsetOfTypedData()), type);
        for (unsigned i = 0; i < maxViews; ++i) {
            jit.load16(Address(type, OBJECT_OFFSETOF(RuntimeType, viewsCount)), other);
            hasNoMoreViews.append(jit.branch32(CCallHelpers::BelowOrEqual, other, TrustedImm32(i)));
            jit.load32(Address(type, sizeof(RuntimeType) + i * sizeof(int32_t)), scratch);
            jit.signExtend32ToPtr(scratch, scratch);
            jit.addPtr(type, scratch);
            miss.append(jit.branch32(CCallHelpers::NotEqual, Address(scratch, OBJECT_OFFSETOF(RuntimeShapeTest, set)), TrustedImm32(-1)));
            jit.load32(Address(scratch, OBJECT_OFFSETOF(RuntimeShapeTest, first)), other);
            jit.add32(Address(scratch, OBJECT_OFFSETOF(RuntimeShapeTest, count)), other);
            jit.sub32(TrustedImm32(1), other);
            jit.add32(Address(A1, Typed::offsetOfShapeWord), other);
            isAccepted.append(jit.branch32(CCallHelpers::Below, other, Address(scratch, OBJECT_OFFSETOF(RuntimeShapeTest, count))));
        }
        jit.load16(Address(type, OBJECT_OFFSETOF(RuntimeType, viewsCount)), other);
        miss.append(jit.branch32(CCallHelpers::Above, other, TrustedImm32(maxViews)));
        hasNoMoreViews.link(&jit);
        jit.lshift32(TrustedImm32(2), other);
        jit.addPtr(type, other);
        jit.load8(Address(A1, JSCell::typeInfoTypeOffset()), scratch);
        miss.append(jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(ObjectType)));
        for (unsigned i = 0; i < maxJSTypes; ++i) {
            hasNoMoreJSTypes.append(jit.branch8(CCallHelpers::BelowOrEqual, Address(type, OBJECT_OFFSETOF(RuntimeType, jsTypesCount)), TrustedImm32(i)));
            isAccepted.append(jit.branch32(CCallHelpers::Equal, scratch, Address(other, sizeof(RuntimeType) + i * sizeof(uint32_t))));
        }
        miss.append(jit.branch8(CCallHelpers::Above, Address(type, OBJECT_OFFSETOF(RuntimeType, jsTypesCount)), TrustedImm32(maxJSTypes)));
        hasNoMoreJSTypes.link(&jit);
        miss.append(jit.branchTest8(CCallHelpers::Zero, Address(type, OBJECT_OFFSETOF(RuntimeType, bits)), TrustedImm32(RuntimeType::allowsAnyFunctionBit)));
        isAccepted.append(jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(JSFunctionType)));
        jit.sub32(TrustedImm32(SlimClosureType), scratch);
        miss.append(jit.branch32(CCallHelpers::Above, scratch, TrustedImm32(SlimClosureWithPropertiesType - SlimClosureType)));
        isAccepted.link(&jit);
    };
    auto didStore = [&](bool mayBeCell) {
        if (created)
            created->append(jit.jump());
        else if (mayBeCell)
            returnAfterStoring(jit, A1);
        else
            jit.ret();
    };
    if (!created) {
        jit.load32(Address(R0, Typed::offsetOfOrderWord), scratch);
        miss.append(jit.branchTest32(CCallHelpers::NonZero, scratch, TrustedImm32(Typed::orderWordIsFrozen)));
    }
    static_assert(!((((1u << FieldWrite::typeBitsInUpperHalf) - 1) << FieldWrite::typeShift) & (FieldWrite::isFieldWrite | FieldWrite::isInGlobalProxyTarget | Slot::isStoreTransition >> 32)));
    static_assert(FieldWrite::offsetBits == 16);
    jit.zeroExtend16To32(T13, address);
    jit.addPtr(R0, address);
    static_assert(!FieldWrite::Boxed);
    Jump isNotBoxed = jit.branchTest32(CCallHelpers::NonZero, T14, TrustedImm32(FieldWrite::keptMask));
    {
        if (!created)
            miss.append(jit.branchTest64(CCallHelpers::Zero, Address(address)));
        CCallHelpers::JumpList isAccepted;
        Jump isNoInt32 = jit.branchIfNotInt32(A1);
        unlessAccepts(FieldWrite::acceptsInt32);
        isAccepted.append(jit.jump());
        isNoInt32.link(&jit);
        Jump isNoNumber = jit.branchIfNotNumber(A1);
        unlessAccepts(FieldWrite::acceptsNumber);
        isAccepted.append(jit.jump());
        isNoNumber.link(&jit);
        Jump isCell = jit.branchIfCell(A1);
        Jump isUndefined = isValue(JSValue::ValueUndefined);
        Jump isNull = isValue(JSValue::ValueNull);
        unlessAccepts(FieldWrite::acceptsBoolean);
        isAccepted.append(isValue(JSValue::ValueTrue));
        isAccepted.append(isValue(JSValue::ValueFalse));
        miss.append(jit.jump());
        isUndefined.link(&jit);
        unlessAccepts(FieldWrite::acceptsUndefined);
        isAccepted.append(jit.jump());
        isNull.link(&jit);
        unlessAccepts(FieldWrite::acceptsNull);
        isAccepted.append(jit.jump());
        isCell.link(&jit);
        jit.load8(Address(A1, JSCell::typeInfoTypeOffset()), scratch);
        Jump isObject = jit.branch32(CCallHelpers::AboveOrEqual, scratch, TrustedImm32(ObjectType));
        Jump isString = jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(StringType));
        Jump isSymbol = jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(SymbolType));
        miss.append(jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(HeapBigIntType)));
        unlessAccepts(FieldWrite::acceptsBigInt);
        isAccepted.append(jit.jump());
        isObject.link(&jit);
        isAccepted.append(jit.branchTest32(CCallHelpers::NonZero, T14, TrustedImm32(FieldWrite::acceptsObject)));
        unlessTypeAcceptsObject();
        isAccepted.append(jit.jump());
        isString.link(&jit);
        unlessAccepts(FieldWrite::acceptsString);
        isAccepted.append(jit.jump());
        isSymbol.link(&jit);
        unlessAccepts(FieldWrite::acceptsSymbol);
        isAccepted.link(&jit);
        jit.store64(A1, Address(address));
        didStore(true);
    }

    isNotBoxed.link(&jit);
    jit.and32(TrustedImm32(FieldWrite::keptMask), T14, scratch);
    Jump isInt32OrElsewhere = jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(FieldWrite::Int32OrElsewhere));
    Jump isCell32 = jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(FieldWrite::Cell32));
    Jump isDouble = jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(FieldWrite::Double));
    Jump isBoolean = jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(FieldWrite::Boolean));
    Jump isInt64 = jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(FieldWrite::Int64));
    static_assert(FieldWrite::Int32 + 1 == FieldWrite::keptMask);
    Jump isInt32 = jit.jump();
    Jump isInBox;

    isInt32OrElsewhere.link(&jit);
    {
        static_assert(Typed::absentInt32 < Typed::undefinedInt32 && Typed::undefinedInt32 < Typed::nullInt32);
        Jump hasBox;
        if (!created) {
            jit.load32(Address(address), scratch);
            Jump isNumberInPlace = jit.branch32(CCallHelpers::GreaterThanOrEqual, scratch, TrustedImm32(Typed::leastInt32InPlace));
            miss.append(jit.branch32(CCallHelpers::LessThanOrEqual, scratch, TrustedImm32(Typed::absentInt32)));
            hasBox = jit.branch32(CCallHelpers::GreaterThan, scratch, TrustedImm32(Typed::nullInt32));
            isNumberInPlace.link(&jit);
        }
        miss.append(jit.branchIfNotInt32(A1));
        unlessAccepts(FieldWrite::acceptsInt32);
        miss.append(jit.branch32(CCallHelpers::LessThan, A1, TrustedImm32(Typed::leastInt32InPlace)));
        jit.store32(A1, Address(address));
        didStore(false);
        if (!created) {
            hasBox.link(&jit);
            jit.and32(TrustedImm32(INT32_MAX), scratch, address);
            jit.lshift64(TrustedImm32(Typed::numberBoxShift), address);
            jit.add64(Address(instanceGPR, Instance::offsetOfNumberBoxBase()), address);
            isInBox = jit.jump();
        }
    }

    isCell32.link(&jit);
    {
        static_assert(!Typed::absentCell32 && Typed::nullCell32 == JSValue::ValueNull && Typed::undefinedCell32 == JSValue::ValueUndefined);
        if (!created)
            miss.append(jit.branchTest32(CCallHelpers::Zero, Address(address)));
        Jump isNoCell = jit.branchIfNotCell(A1);
        jit.load8(Address(A1, JSCell::typeInfoTypeOffset()), scratch);
        Jump isString = jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(StringType));
        Jump isObject = jit.branch32(CCallHelpers::AboveOrEqual, scratch, TrustedImm32(ObjectType));
        miss.append(jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(SymbolType)));
        unlessAccepts(FieldWrite::acceptsSymbol);
        Jump isAcceptedSymbol = jit.jump();
        isObject.link(&jit);
        miss.append(jit.branchTestPtr(CCallHelpers::NonZero, A1, TrustedImm32(PreciseAllocation::halfAlignment)));
        unlessTypeAcceptsObject();
        Jump isAcceptedObject = jit.jump();
        isString.link(&jit);
        unlessAccepts(FieldWrite::acceptsString);
        isAcceptedSymbol.link(&jit);
        isAcceptedObject.link(&jit);
        jit.urshift64(A1, TrustedImm32(Typed::cell32Shift), scratch);
        jit.store32(scratch, Address(address));
        didStore(true);

        isNoCell.link(&jit);
        Jump isUndefined = isValue(JSValue::ValueUndefined);
        miss.append(jit.branch64(CCallHelpers::NotEqual, A1, CCallHelpers::TrustedImm64(JSValue::ValueNull)));
        unlessAccepts(FieldWrite::acceptsNull);
        Jump isAcceptedNull = jit.jump();
        isUndefined.link(&jit);
        unlessAccepts(FieldWrite::acceptsUndefined);
        isAcceptedNull.link(&jit);
        jit.store32(A1, Address(address));
        didStore(false);
    }

    isDouble.link(&jit);
    {
        if (!created) {
            Jump hasNoMarkers = jit.branchTest32(CCallHelpers::Zero, T14, TrustedImm32(FieldWrite::isOptional));
            jit.load64(Address(address), scratch);
            miss.append(jit.branch64(CCallHelpers::Equal, scratch, CCallHelpers::TrustedImm64(static_cast<int64_t>(Typed::absentDouble))));
            hasNoMarkers.link(&jit);
            isInBox.link(&jit);
        }
        Jump isNoInt32 = jit.branchIfNotInt32(A1);
        unlessAccepts(FieldWrite::acceptsInt32);
        jit.convertInt32ToDouble(A1, FPRInfo::fpRegT0);
        jit.storeDouble(FPRInfo::fpRegT0, Address(address));
        didStore(false);
        isNoInt32.link(&jit);
        miss.append(jit.branchIfNotNumber(A1));
        unlessAccepts(FieldWrite::acceptsNumber);
        jit.add64(numberTag, A1, scratch);
        jit.store64(scratch, Address(address));
        didStore(false);
    }

    isBoolean.link(&jit);
    {
        Jump hasNoMarkers = jit.branchTest32(CCallHelpers::Zero, T14, TrustedImm32(FieldWrite::isOptional));
        // The byte is the low one of the JSValue.
        if (!created)
            miss.append(jit.branchTest8(CCallHelpers::Zero, Address(address)));
        Jump isUndefined = isValue(JSValue::ValueUndefined);
        Jump isNull = isValue(JSValue::ValueNull);
        jit.and64(TrustedImm32(~1), A1, scratch);
        miss.append(jit.branch64(CCallHelpers::NotEqual, scratch, CCallHelpers::TrustedImm64(JSValue::ValueFalse)));
        unlessAccepts(FieldWrite::acceptsBoolean);
        Jump isAcceptedBoolean = jit.jump();
        isUndefined.link(&jit);
        unlessAccepts(FieldWrite::acceptsUndefined);
        Jump isAcceptedUndefined = jit.jump();
        isNull.link(&jit);
        unlessAccepts(FieldWrite::acceptsNull);
        isAcceptedBoolean.link(&jit);
        isAcceptedUndefined.link(&jit);
        jit.store8(A1, Address(address));
        didStore(false);

        hasNoMarkers.link(&jit);
        jit.and64(TrustedImm32(~1), A1, scratch);
        miss.append(jit.branch64(CCallHelpers::NotEqual, scratch, CCallHelpers::TrustedImm64(JSValue::ValueFalse)));
        unlessAccepts(FieldWrite::acceptsBoolean);
        jit.and32(TrustedImm32(1), A1, scratch);
        jit.store8(scratch, Address(address));
        didStore(false);
    }

    isInt64.link(&jit);
    {
        if (!created) {
            Jump hasNoMarkers = jit.branchTest32(CCallHelpers::Zero, T14, TrustedImm32(FieldWrite::isOptional));
            jit.load64(Address(address), scratch);
            miss.append(jit.branch64(CCallHelpers::Equal, scratch, CCallHelpers::TrustedImm64(Typed::absentInteger)));
            hasNoMarkers.link(&jit);
        }
        Jump isNoInt32 = jit.branchIfNotInt32(A1);
        unlessAccepts(FieldWrite::acceptsInt32);
        jit.signExtend32ToPtr(A1, scratch);
        jit.store64(scratch, Address(address));
        didStore(false);
        isNoInt32.link(&jit);
        Jump isUndefined = isValue(JSValue::ValueUndefined);
        miss.append(jit.branch64(CCallHelpers::NotEqual, A1, CCallHelpers::TrustedImm64(JSValue::ValueNull)));
        unlessAccepts(FieldWrite::acceptsNull);
        jit.move(CCallHelpers::TrustedImm64(Typed::nullInteger), scratch);
        Jump isAcceptedNull = jit.jump();
        isUndefined.link(&jit);
        unlessAccepts(FieldWrite::acceptsUndefined);
        jit.move(CCallHelpers::TrustedImm64(Typed::undefinedInteger), scratch);
        isAcceptedNull.link(&jit);
        jit.store64(scratch, Address(address));
        didStore(false);
    }

    isInt32.link(&jit);
    {
        miss.append(jit.branchIfNotInt32(A1));
        unlessAccepts(FieldWrite::acceptsInt32);
        jit.store32(A1, Address(address));
        didStore(false);
    }
}

// The site has seen a store by its name make an object of the structure one of a specialization that has a field by the name: Typed::StoreTransition says all there is to do. R0: the object, whose structure is that of the slot. A1: the value.
// T13: Slot::pointer, with Slot::isStoreTransition. R0, A1 and A2 are kept. Until the structure is stored nothing is written that the object's layout knows of: whoever leaves for `miss` leaves the object as it was.
static void storeByTransition(CCallHelpers& jit, CCallHelpers::JumpList& miss)
{
    using Transition = Typed::StoreTransition;
    constexpr GPRReg transition = T10;
    auto in = [&](ptrdiff_t offset) {
        return Address(transition, offset);
    };
    static_assert(Slot::isStoreTransition == 1ull << 62);
    jit.move(T13, transition);
    jit.lshift64(TrustedImm32(2), transition);
    jit.urshift64(TrustedImm32(2), transition);
    // This order and count of keys, this shape, no extra properties, not frozen, not sealed, extensible.
    jit.load64(Address(R0, Typed::offsetOfOrderWord), T11);
    miss.append(jit.branch64(CCallHelpers::NotEqual, T11, in(OBJECT_OFFSETOF(Transition, wordsBefore))));
    // The cell has the room.
    miss.append(jit.branchTestPtr(CCallHelpers::NonZero, R0, TrustedImm32(PreciseAllocation::halfAlignment)));
    jit.move(R0, T11);
    jit.andPtr(CCallHelpers::TrustedImmPtr(MarkedBlock::blockMask), T11);
    jit.loadPtr(Address(T11, MarkedBlock::offsetOfHeader + MarkedBlock::Header::offsetOfHandle()), T11);
    jit.load32(Address(T11, MarkedBlock::Handle::offsetOfAtomsPerCell()), T11);
    static_assert(MarkedBlock::atomSize == 16 && !MarkedBlock::Header::offsetOfHandle() && !MarkedBlock::Handle::offsetOfAtomsPerCell());
    jit.lshift32(TrustedImm32(4), T11);
    miss.append(jit.branch32(CCallHelpers::Below, T11, in(OBJECT_OFFSETOF(Transition, size))));
    // Nothing on the prototype chain takes or refuses the store.
    CCallHelpers::JumpList chainEnds;
    for (unsigned i = 0; i < Transition::maxHolders; ++i) {
        jit.loadPtr(in(OBJECT_OFFSETOF(Transition, holders) + i * sizeof(JSCell*)), T11);
        chainEnds.append(jit.branchTestPtr(CCallHelpers::Zero, T11));
        jit.load32(Address(T11, JSCell::structureIDOffset()), T11);
        miss.append(jit.branch32(CCallHelpers::NotEqual, T11, in(OBJECT_OFFSETOF(Transition, structureIDsOfHolders) + i * sizeof(uint32_t))));
    }
    chainEnds.link(&jit);
    // The slots that the new layout adds say "absent". The field's own is among them.
    {
        using Cleared = Transition::Cleared;
        constexpr GPRReg remaining = T11;
        constexpr GPRReg cleared = T12;
        constexpr GPRReg address = T13;
        constexpr GPRReg word = T14;
        jit.load32(in(OBJECT_OFFSETOF(Transition, numberOfCleared)), remaining);
        jit.loadPtr(in(OBJECT_OFFSETOF(Transition, cleared)), cleared);
        auto next = jit.label();
        Jump areCleared = jit.branchTest32(CCallHelpers::Zero, remaining);
        jit.load32(Address(cleared, OBJECT_OFFSETOF(Cleared, offset)), address);
        jit.addPtr(R0, address);
        jit.load32(Address(cleared, OBJECT_OFFSETOF(Cleared, width)), word);
        Jump isEightBytes = jit.branch32(CCallHelpers::Equal, word, TrustedImm32(8));
        Jump isFourBytes = jit.branch32(CCallHelpers::Equal, word, TrustedImm32(4));
        jit.load64(Address(cleared, OBJECT_OFFSETOF(Cleared, absent)), word);
        jit.store8(word, Address(address));
        Jump didClearByte = jit.jump();
        isFourBytes.link(&jit);
        jit.load64(Address(cleared, OBJECT_OFFSETOF(Cleared, absent)), word);
        jit.store32(word, Address(address));
        Jump didClearFourBytes = jit.jump();
        isEightBytes.link(&jit);
        jit.load64(Address(cleared, OBJECT_OFFSETOF(Cleared, absent)), word);
        jit.store64(word, Address(address));
        didClearByte.link(&jit);
        didClearFourBytes.link(&jit);
        jit.addPtr(TrustedImm32(sizeof(Cleared)), cleared);
        jit.sub32(TrustedImm32(1), remaining);
        jit.jump().linkTo(next, &jit);
        areCleared.link(&jit);
    }
    // The value is tested and stored as to a field of the new layout.
    jit.load64(in(OBJECT_OFFSETOF(Transition, fieldWrite)), T13);
    jit.load32(in(OBJECT_OFFSETOF(Transition, fieldWrite) + sizeof(uint32_t)), T14);
    CCallHelpers::JumpList created;
    storeCompiledField(jit, miss, &created);
    created.link(&jit);
    // The collector goes by the structure, which comes behind everything it will look at.
    jit.storeFence();
    jit.load32(in(OBJECT_OFFSETOF(Transition, structureID)), T11);
    jit.store32(T11, Address(R0, JSCell::structureIDOffset()));
    jit.load64(in(OBJECT_OFFSETOF(Transition, wordsAfter)), T11);
    jit.store64(T11, Address(R0, Typed::offsetOfOrderWord));
    returnAfterStoring(jit, A1);
}

static void generatePutById(CCallHelpers& jit)
{
    CCallHelpers::JumpList miss;
    CCallHelpers::JumpList isNoField;
    Jump isNoCell = jit.branchIfNotCell(R0);
    jit.load64(slotWord(A2, 0), T11);
    miss.append(branchIfStructureDiffers(jit, R0, T11));
    jit.load64(slotWord(A2, 1), T13);
    Jump isOutOfLine;
    {
        Jump hasPointerWord = jit.branchTest64(CCallHelpers::NonZero, T13);
        isOutOfLine = jit.branchTest64(CCallHelpers::NonZero, T11, CCallHelpers::TrustedImm64(1ll << (32 + Slot::offsetBits - 1)));
        extractBits(jit, T11, 32, Slot::offsetBits, T12);
        jit.store64(A1, CCallHelpers::BaseIndex(R0, T12, CCallHelpers::TimesEight));
        returnAfterStoring(jit, A1);
        hasPointerWord.link(&jit);
    }
#if CPU(X86_64)
    Jump slotHasFieldType = jit.branchTest32(CCallHelpers::NonZero, slotWord(A2, 1).withOffset(sizeof(uint32_t)));
#else
    jit.urshift64(T13, TrustedImm32(32), T14);
    Jump slotHasFieldType = jit.branchTest32(CCallHelpers::NonZero, T14);
#endif
    locateCachedProperty(jit, R0, T11, T12);
    jit.store64(A1, CCallHelpers::BaseIndex(T12, T11, CCallHelpers::TimesEight));
    jit.store32(T13, Address(R0, JSCell::structureIDOffset()));
    Jump addsProperty = jit.jump();
    isOutOfLine.link(&jit);
    CCallHelpers::Label isHeld = jit.label();
    locateCachedProperty(jit, R0, T11, T12);
    jit.store64(A1, CCallHelpers::BaseIndex(T12, T11, CCallHelpers::TimesEight));
    Jump sameStructure = jit.branchTest32(CCallHelpers::Zero, T13);
    jit.store32(T13, Address(R0, JSCell::structureIDOffset()));
    Jump hasDifferentStructure = jit.jump();
    sameStructure.link(&jit);

    CCallHelpers::Label stored = jit.label();
    CCallHelpers::Label storedWithPossiblyDifferentStructure = returnAfterStoring(jit, A1);
    hasDifferentStructure.linkTo(storedWithPossiblyDifferentStructure, &jit);
    addsProperty.linkTo(storedWithPossiblyDifferentStructure, &jit);

    {
        slotHasFieldType.link(&jit);
#if CPU(X86_64)
        jit.load32(slotWord(A2, 1).withOffset(sizeof(uint32_t)), T14);
#endif
        {
            Jump isNoCompiledField = branchUnlessSlotHas<Slot::pointerIsNotCell>(jit, T11);
            static_assert(!(Slot::FieldWrite::isFieldWrite & Slot::isStoreTransition >> 32));
            Jump makesTransition = jit.branchTest32(CCallHelpers::NonZero, T14, TrustedImm32(static_cast<int32_t>(Slot::isStoreTransition >> 32)));
            jit.and32(TrustedImm32(Slot::FieldWrite::isFieldWrite | Slot::FieldWrite::isNoField), T14, T12);
            Jump isNoFieldOfLayout = jit.branch32(CCallHelpers::Equal, T12, TrustedImm32(Slot::FieldWrite::isFieldWrite | Slot::FieldWrite::isNoField));
            storeCompiledField(jit, miss);
            makesTransition.link(&jit);
            storeByTransition(jit, miss);
            isNoFieldOfLayout.link(&jit);
            isNoField.append(jit.branchTest32(CCallHelpers::Zero, T14, TrustedImm32(Slot::FieldWrite::isInGlobalProxyTarget)));
            jit.loadPtr(Address(R0, JSGlobalProxy::targetOffset()), R0);
            locateCachedProperty(jit, R0, T11, T12);
            jit.store64(A1, CCallHelpers::BaseIndex(T12, T11, CCallHelpers::TimesEight));
            jit.jump().linkTo(stored, &jit);
            isNoCompiledField.link(&jit);
        }
        auto ifAccepts = [&](unsigned kind) {
            miss.append(jit.branchTest32(CCallHelpers::Zero, T14, TrustedImm32(kind)));
            jit.jump().linkTo(isHeld, &jit);
        };
        Jump isCell = jit.branchIfCell(A1);
        Jump isNotNumber = jit.branchIfNotNumber(A1);
        miss.append(jit.branchTest32(CCallHelpers::Zero, T14, TrustedImm32(SoundTypeNumber)));
        jit.branchIfNotInt32(A1).linkTo(isHeld, &jit);
        jit.convertInt32ToDouble(A1, FPRInfo::fpRegT0);
        jit.moveDoubleTo64(FPRInfo::fpRegT0, A1);
        jit.sub64(numberTag, A1);
        jit.jump().linkTo(isHeld, &jit);

        isNotNumber.link(&jit);
        Jump isNotUndefined = jit.branch64(CCallHelpers::NotEqual, A1, CCallHelpers::TrustedImm64(JSValue::ValueUndefined));
        ifAccepts(SoundTypeUndefined);
        isNotUndefined.link(&jit);
        Jump isNotNull = jit.branch64(CCallHelpers::NotEqual, A1, CCallHelpers::TrustedImm64(JSValue::ValueNull));
        ifAccepts(SoundTypeNull);
        isNotNull.link(&jit);
        jit.and64(TrustedImm32(~1), A1, T12);
        miss.append(jit.branch64(CCallHelpers::NotEqual, T12, CCallHelpers::TrustedImm64(JSValue::ValueFalse)));
        ifAccepts(SoundTypeBoolean);

        isCell.link(&jit);
        jit.load8(Address(A1, JSCell::typeInfoTypeOffset()), T12);
        Jump isNotString = jit.branch32(CCallHelpers::NotEqual, T12, TrustedImm32(StringType));
        ifAccepts(SoundTypeString);
        isNotString.link(&jit);
        Jump isNotArray = jit.branch32(CCallHelpers::NotEqual, T12, TrustedImm32(ArrayType));
        ifAccepts(SoundTypeArray);
        isNotArray.link(&jit);
        Jump isNotFunction = jit.branch32(CCallHelpers::NotEqual, T12, TrustedImm32(JSFunctionType));
        ifAccepts(SoundTypeFunction);
        isNotFunction.link(&jit);
        Jump isFinalObject = jit.branch32(CCallHelpers::Equal, T12, TrustedImm32(FinalObjectType));
        miss.append(jit.branch32(CCallHelpers::Below, T12, TrustedImm32(ObjectType)));
        miss.append(jit.branch32(CCallHelpers::Equal, T12, TrustedImm32(InternalFunctionType)));
        miss.append(jit.branch32(CCallHelpers::Equal, T12, TrustedImm32(DerivedArrayType)));
        jit.load8(Address(A1, JSCell::typeInfoFlagsOffset()), T10);
        Jump mayBeSlimClosure = jit.branchTest32(CCallHelpers::NonZero, T10, TrustedImm32(OverridesGetCallData));
        jit.urshift32(T14, TrustedImm32(16), T10);
        Jump goesByKind = jit.branchTest32(CCallHelpers::Zero, T10);
        miss.append(jit.branchTest32(CCallHelpers::NonZero, T14, TrustedImm32(SoundTypeOtherObject)));
        jit.or32(TrustedImm32(static_cast<int32_t>(0xffff0000)), T10);
        jit.load32(Address(A1, Typed::offsetOfShapeWord), T12);
        jit.branch32(CCallHelpers::Equal, T10, T12).linkTo(isHeld, &jit);
        miss.append(jit.jump());
        goesByKind.link(&jit);
        ifAccepts(SoundTypeOtherObject);
        mayBeSlimClosure.link(&jit);
        miss.append(jit.branch32(CCallHelpers::Below, T12, TrustedImm32(SlimClosureType)));
        miss.append(jit.branch32(CCallHelpers::Above, T12, TrustedImm32(SlimClosureWithPropertiesType)));
        ifAccepts(SoundTypeFunction);
        isFinalObject.link(&jit);
        miss.append(jit.branchTest32(CCallHelpers::Zero, T14, TrustedImm32(SoundTypeOtherObject)));
        jit.urshift32(T14, TrustedImm32(16), T12);
        jit.branchTest32(CCallHelpers::Zero, T12).linkTo(isHeld, &jit);
        miss.append(jit.jump());
    }

    miss.link(&jit);
    loadCallerIndex(jit);
    loadDataForSlot(jit, A2, T10);
    countSlotMiss(jit, T10);
    {
        // What has a put of its own is served by none of the tables below.
        jit.load32(Address(R0, JSCell::structureIDOffset()), T11);
        structureWithID(jit, T11);
        static_assert(sizeof(TypeInfo::OutOfLineTypeFlags) == sizeof(uint16_t));
        jit.load16(Address(T11, Structure::outOfLineTypeFlagsOffset()), T11);
        Jump hasNoPutOfItsOwn = jit.branchTest32(CCallHelpers::Zero, T11, TrustedImm32(OverridesPut >> 8));
        // An object of a compiled layout is one of them, and the field may be known: see Instance::NamedField. A store that defines does not ask.
        // Only for a slot that cannot learn: it has given up, or it is nobody's, which reads the same. One that can goes on to the operation, which fills it.
        jit.load32(Address(A2, OBJECT_OFFSETOF(Slot, offset)), T11);
        jit.and32(TrustedImm32(Slot::attemptsMask), T11);
        isNoField.append(jit.branch32(CCallHelpers::NotEqual, T11, TrustedImm32(Slot::attemptsMask)));
        slotSite(jit, T10, A2, T13);
        loadInfo(jit, T10);
        loadSites(jit, T10, T11);
        jit.load32(CCallHelpers::BaseIndex(T11, T13, CCallHelpers::TimesFour), T12);
        isNoField.append(jit.branchTest32(CCallHelpers::NonZero, T12, TrustedImm32(1u << Site::identifierBits)));
        jit.and32(TrustedImm32((1u << Site::identifierBits) - 1), T12);
        findNamedField(jit, Instance::offsetOfNamedFieldWrites(), T13, isNoField);
        jit.urshift64(T13, TrustedImm32(32), T14);
        storeCompiledField(jit, isNoField);
        hasNoPutOfItsOwn.link(&jit);
    }
    CCallHelpers::JumpList notFound;
    constexpr bool keepsCallerIndex = T15 != T9;
    {
#if CPU(X86_64)
        constexpr GPRReg cache = T10;
#else
        constexpr GPRReg cache = GPRInfo::argumentGPR7;
#endif
        notFound.append(jit.branchIfNotObject(R0));
        slotSite(jit, T10, A2, T13);
        loadInfo(jit, T10);
        loadSites(jit, T10, T11);
        jit.load32(CCallHelpers::BaseIndex(T11, T13, CCallHelpers::TimesFour), T12);
        notFound.append(jit.branchTest32(CCallHelpers::NonZero, T12, TrustedImm32(Site::isOfRowOfThis << Site::identifierBits)));
        {
            CCallHelpers::JumpList isOfNoRow;
            isOfNoRow.append(jit.branchTest32(CCallHelpers::NonZero, T12, TrustedImm32((((1u << Site::extraBits) - 1) & ~2u) << Site::identifierBits)));
            jit.load32(Address(A2, OBJECT_OFFSETOF(Slot, offset)), T11);
            jit.and32(TrustedImm32(Slot::attemptsMask), T11);
            isOfNoRow.append(jit.branch32(CCallHelpers::NotEqual, T11, TrustedImm32(Slot::attemptsMask)));
            jit.and32(TrustedImm32((1u << Site::identifierBits) - 1), T12, T13);
            findPlaceByNameAndRow(jit, T13, T11, A3, T10, isOfNoRow, isOfNoRow);
            jit.store64(A1, CCallHelpers::BaseIndex(R0, T10, CCallHelpers::TimesEight, JSObject::offsetOfInlineStorage()));
            returnAfterStoring(jit, A1);
            isOfNoRow.link(&jit);
        }
        auto loadNameAndCache = [&] {
            jit.and32(TrustedImm32((1u << Site::identifierBits) - 1), T12);
            jit.loadPtr(Address(instanceGPR, Instance::offsetOfProgramIdentifierPlaces()), T11);
            jit.load16(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesTwo), T12);
            jit.loadPtr(Address(instanceGPR, Instance::offsetOfProgramIdentifiers()), T11);
            jit.loadPtr(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesEight), A3);
            notFound.append(jit.branchTestPtr(CCallHelpers::Zero, A3));
            jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), cache);
            jit.loadPtr(Address(cache, static_cast<unsigned>(Entry::MegamorphicCache) * sizeof(void*)), cache);
            notFound.append(jit.branchTestPtr(CCallHelpers::Zero, cache));
        };
        Jump isNotDirect = jit.branchTest32(CCallHelpers::Zero, T12, TrustedImm32(1u << Site::identifierBits));
        Jump storesNoLazyClosure = jit.branchTest32(CCallHelpers::Zero, T12, TrustedImm32(Site::storesLazyClosure << Site::identifierBits));
        {
            using StoreEntry = MegamorphicCache::StoreEntry;
            CCallHelpers::JumpList isNotInPrimary;
            loadNameAndCache();
            jit.load32(Address(R0, JSCell::structureIDOffset()), T11);
            jit.urshift32(T11, TrustedImm32(MegamorphicCache::structureIDHashShift1), T12);
            jit.urshift32(T11, TrustedImm32(MegamorphicCache::structureIDHashShift4), T13);
            jit.xor32(T12, T13);
            jit.load32(Address(A3, UniquedStringImpl::flagsOffset()), T12);
            jit.urshift32(TrustedImm32(StringImpl::s_flagCount), T12);
            jit.add32(T12, T13);
            jit.and32(TrustedImm32(MegamorphicCache::storeCachePrimaryMask), T13);
            jit.mul32(TrustedImm32(sizeof(StoreEntry)), T13, T13);
            jit.addPtr(cache, T13);
            jit.addPtr(TrustedImm32(MegamorphicCache::offsetOfStoreCachePrimaryEntries()), T13);
            jit.load16(Address(cache, MegamorphicCache::offsetOfEpoch()), T12);
            isNotInPrimary.append(jit.branch32(CCallHelpers::NotEqual, T11, Address(T13, StoreEntry::offsetOfOldStructureID())));
            isNotInPrimary.append(jit.branchPtr(CCallHelpers::NotEqual, Address(T13, StoreEntry::offsetOfUid()), A3));
            CCallHelpers::Label isFound = jit.label();
            notFound.append(jit.branch32WithMemory16(CCallHelpers::NotEqual, Address(T13, StoreEntry::offsetOfEpoch()), T12));
            notFound.append(jit.branch8(CCallHelpers::NotEqual, Address(T13, StoreEntry::offsetOfReallocating()), TrustedImm32(StoreEntry::addsLazyClosure)));
            jit.load32(Address(T13, StoreEntry::offsetOfNewStructureID()), T12);
            jit.load16(Address(T13, StoreEntry::offsetOfOffset()), T13);
            jit.store32(T12, Address(R0, JSCell::structureIDOffset()));
            jit.storeProperty(A1, R0, T13, T11);
            jit.jump().linkTo(storedWithPossiblyDifferentStructure, &jit);

            isNotInPrimary.link(&jit);
            jit.add32(A3, T11, T13);
            jit.urshift32(T13, TrustedImm32(MegamorphicCache::structureIDHashShift5), T12);
            jit.add32(T12, T13);
            jit.and32(TrustedImm32(MegamorphicCache::storeCacheSecondaryMask), T13);
            jit.mul32(TrustedImm32(sizeof(StoreEntry)), T13, T13);
            jit.addPtr(cache, T13);
            jit.addPtr(TrustedImm32(MegamorphicCache::offsetOfStoreCacheSecondaryEntries()), T13);
            jit.load16(Address(cache, MegamorphicCache::offsetOfEpoch()), T12);
            notFound.append(jit.branch32(CCallHelpers::NotEqual, T11, Address(T13, StoreEntry::offsetOfOldStructureID())));
            notFound.append(jit.branchPtr(CCallHelpers::NotEqual, Address(T13, StoreEntry::offsetOfUid()), A3));
            jit.jump().linkTo(isFound, &jit);
        }
        storesNoLazyClosure.link(&jit);
        jit.load32(Address(R0, JSCell::structureIDOffset()), T11);
        structureWithID(jit, T11);
        notFound.append(jit.branchTest32(CCallHelpers::NonZero, Address(T11, Structure::bitFieldOffset()), TrustedImm32(Structure::s_hasNonEnumerablePropertiesBits | Structure::s_hasNonConfigurablePropertiesBits)));
        isNotDirect.link(&jit);
        loadNameAndCache();
        Jump isTaken = jit.branchTest32(CCallHelpers::NonZero, Address(A2, OBJECT_OFFSETOF(Slot, structureID)));
        jit.load32(Address(A2, OBJECT_OFFSETOF(Slot, offset)), T11);
        jit.and32(TrustedImm32(Slot::attemptsMask), T11);
        Jump isUntried = jit.branchTest32(CCallHelpers::Zero, T11);
        Jump shouldBeFilled = jit.branch32(CCallHelpers::NotEqual, T11, TrustedImm32(Slot::attemptsMask));
        isTaken.link(&jit);
        isUntried.link(&jit);
        auto [slow, reallocating] = jit.storeMegamorphicProperty(CCallHelpers::MegamorphicCacheLocation(cache), R0, A3, nullptr, A1, T11, T12, T13);
        jit.load32(Address(A2, OBJECT_OFFSETOF(Slot, offset)), T11);
        jit.branchTest32(CCallHelpers::NonZero, T11, TrustedImm32(Slot::attemptsMask)).linkTo(storedWithPossiblyDifferentStructure, &jit);
        jit.branchTest32(CCallHelpers::NonZero, Address(A2, OBJECT_OFFSETOF(Slot, structureID))).linkTo(storedWithPossiblyDifferentStructure, &jit);
        jit.add32(TrustedImm32(1u << Slot::attemptsShift), T11);
        jit.store32(T11, Address(A2, OBJECT_OFFSETOF(Slot, offset)));
        jit.jump().linkTo(storedWithPossiblyDifferentStructure, &jit);

        reallocating.link(&jit);
        {
            using StoreEntry = MegamorphicCache::StoreEntry;
            constexpr size_t storageSize = initialOutOfLineCapacity * sizeof(EncodedJSValue);
            constexpr GPRReg entry = T13;
            constexpr GPRReg allocator = T10;
            constexpr GPRReg storage = T11;
            notFound.append(jit.branch8(CCallHelpers::NotEqual, Address(entry, StoreEntry::offsetOfReallocating()), TrustedImm32(StoreEntry::allocatesInitialOutOfLineStorage)));
            notFound.append(jit.branchTestPtr(CCallHelpers::NonZero, Address(R0, JSObject::butterflyOffset())));
            jit.loadPtr(Address(instanceGPR, Instance::offsetOfAuxiliarySpace()), allocator);
            jit.loadPtr(Address(allocator, CompleteSubspace::offsetOfAllocatorForSizeStep() + MarkedSpace::sizeClassToIndex(storageSize) * sizeof(Allocator)), allocator);
            notFound.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
            jit.emitAllocateWithNonNullAllocator(storage, JITAllocator::variable(), allocator, T12, notFound, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
            for (unsigned i = 0; i + 1 < initialOutOfLineCapacity; ++i)
                jit.store64(TrustedImm32(0), Address(storage, i * sizeof(EncodedJSValue)));
            jit.store64(A1, Address(storage, storageSize - sizeof(EncodedJSValue)));
            jit.addPtr(TrustedImm32(storageSize + sizeof(IndexingHeader)), storage);
            jit.load32(Address(entry, StoreEntry::offsetOfNewStructureID()), T12);
            jit.or32(TrustedImm32(StructureID::nukedStructureIDBit), Address(R0, JSCell::structureIDOffset()));
            jit.storeFence();
            jit.storePtr(storage, Address(R0, JSObject::butterflyOffset()));
            jit.storeFence();
            jit.store32(T12, Address(R0, JSCell::structureIDOffset()));
            jit.jump().linkTo(storedWithPossiblyDifferentStructure, &jit);
        }

        slow.link(&jit);
        shouldBeFilled.link(&jit);
        loadDataForSlot(jit, A2, T11);
        slotSite(jit, T11, A2, T13);
        loadInfo(jit, T11);
        loadSites(jit, T11, T12);
        jit.load32(CCallHelpers::BaseIndex(T12, T13, CCallHelpers::TimesFour), T12);
        notFound.append(jit.branchTest32(CCallHelpers::NonZero, T12, TrustedImm32(1u << Site::identifierBits)));

        using InheritedSetter = Instance::InheritedSetter;
        auto inEntry = [&](ptrdiff_t offset) { return Address(T12, Instance::offsetOfInheritedSetters() + offset); };
        jit.load32(Address(R0, JSCell::structureIDOffset()), T11);
        jit.urshift32(T11, TrustedImm32(4), T12);
        jit.urshift64(A3, TrustedImm32(4), T13);
        jit.xor32(T13, T12);
        jit.and32(TrustedImm32(Instance::numberOfInheritedSetters - 1), T12);
        static_assert(sizeof(InheritedSetter) == 32);
        jit.lshiftPtr(TrustedImm32(5), T12);
        jit.addPtr(instanceGPR, T12);
        notFound.append(jit.branch32(CCallHelpers::NotEqual, T11, inEntry(OBJECT_OFFSETOF(InheritedSetter, structureID))));
        notFound.append(jit.branchPtr(CCallHelpers::NotEqual, inEntry(OBJECT_OFFSETOF(InheritedSetter, uid)), A3));
        jit.load16(Address(cache, MegamorphicCache::offsetOfEpoch()), T11);
        jit.load16(inEntry(OBJECT_OFFSETOF(InheritedSetter, epoch)), T13);
        notFound.append(jit.branch32(CCallHelpers::NotEqual, T11, T13));
        jit.load16(inEntry(OBJECT_OFFSETOF(InheritedSetter, offset)), T13);
        jit.loadPtr(inEntry(OBJECT_OFFSETOF(InheritedSetter, holder)), T12);
        jit.loadProperty(T12, T13, T11);
        jit.loadPtr(Address(T11, GetterSetter::offsetOfSetter()), T12);
        notFound.append(jit.branchTest8(CCallHelpers::Zero, Address(T12, JSCell::typeInfoFlagsOffset()), TrustedImm32(TypeOfYieldsFunction)));
        checkStackBeforeCallingAccessor(jit);
        jit.move(T12, calleeGPR);
        jit.move(R0, thisGPR);
        jit.move(A1, argumentGPR(0));
        jit.move(TrustedImm32(1), countGPR);
        jit.jump().linkTo(variadicCallCellLabel(), &jit);

        if (!keepsCallerIndex)
            notFound.link(&jit);
    }
    isNoField.link(&jit);
    CCallHelpers::Label callsOperation = jit.label();
    missAtSite(jit, Entry::operationAOTPutById, 2, Returns::Void, keepsCallerIndex ? &notFound : nullptr);

    isNoCell.link(&jit);
    loadCallerIndex(jit);
    loadDataForSlot(jit, A2, T10);
    countSlotMiss(jit, T10);
    jit.jump().linkTo(callsOperation, &jit);
}

static void checkPrivateNameCache(CCallHelpers& jit, GPRReg slot, CCallHelpers::JumpList& miss)
{
    miss.append(jit.branchIfNotCell(R0));
    jit.load64(slotWord(slot, 0), T11);
    miss.append(branchIfStructureDiffers(jit, R0, T11));
    miss.append(jit.branchPtr(CCallHelpers::NotEqual, slotWord(slot, 1), A1));
}

// R0 and A1 are cells. See Instance::keyedReads. Where it goes on, T11 is the first word of the entry for the two, which says where the property is.
static void findInKeyedTable(CCallHelpers& jit, ptrdiff_t offsetOfTable, unsigned numberOfEntries, GPRReg entry, CCallHelpers::JumpList& miss, CCallHelpers::JumpList& isNotForTable, unsigned sizeOfEntry = sizeof(Slot))
{
    RELEASE_ASSERT(hasOneBitSet(sizeOfEntry));
    unsigned shift = getLSBSet(sizeOfEntry);
    RELEASE_ASSERT(shift == Instance::keyedKeyShift || shift == Instance::keyedKeyShift + 1);
    jit.load32(Address(R0, JSCell::structureIDOffset()), T12);
    jit.urshift32(T12, TrustedImm32(Instance::keyedStructureShift - shift), T11);
#if CPU(X86_64)
    jit.x86Lea32(CCallHelpers::BaseIndex(T11, A1, shift == Instance::keyedKeyShift ? CCallHelpers::TimesOne : CCallHelpers::TimesTwo), entry);
    jit.and32(TrustedImm32((numberOfEntries - 1) << shift), entry);
    jit.addPtr(Address(instanceGPR, offsetOfTable), entry);
#else
    jit.addLeftShift32(T11, A1, TrustedImm32(shift - Instance::keyedKeyShift), entry);
    jit.and32(TrustedImm32((numberOfEntries - 1) << shift), entry);
    jit.loadPtr(Address(instanceGPR, offsetOfTable), T11);
    jit.addPtr(T11, entry);
#endif
    jit.load64(slotWord(entry, 0), T11);
    miss.append(jit.branch32(CCallHelpers::NotEqual, T11, T12));
    miss.append(jit.branchPtr(CCallHelpers::NotEqual, slotWord(entry, 1), A1));
    isNotForTable.append(branchIfSlotHas<Slot::isGetter>(jit, T11));
}

static void callLookUp(CCallHelpers& jit, Entry operation, GPRReg base, GPRReg name)
{
    jit.emitFunctionPrologue();
#if CPU(ARM64)
    jit.pushPair(R0, A1);
#else
    jit.push(R0);
    jit.push(A1);
#endif
    if (name == InvalidGPRReg)
        jit.loadPtr(Address(A1, JSString::offsetOfValue()), A1);
    else
        jit.move(name, A1);
    jit.move(R0, A0);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T11);
    jit.loadPtr(Address(T11, static_cast<unsigned>(operation) * sizeof(void*)), T11);
    jit.call(T11, OperationPtrTag);
#if CPU(ARM64)
    jit.popPair(base, A1);
#else
    jit.pop(A1);
    jit.pop(base);
#endif
    jit.emitFunctionEpilogue();
}

// By a key that is a cell: a symbol, or a string that is the same cell each time.
static void generateGetByValCached(CCallHelpers& jit)
{
    CCallHelpers::JumpList miss;
    CCallHelpers::JumpList generally;
    generally.append(jit.branchIfNotCell(A1));
    generally.append(jit.branchIfNotCell(R0));
    CCallHelpers::JumpList isNotInObject;
    static_assert(!OBJECT_OFFSETOF(Instance::KeyedRead, slot));
    findInKeyedTable(jit, Instance::offsetOfKeyedReads(), Instance::numberOfKeyedReads, A2, miss, isNotInObject, sizeof(Instance::KeyedRead));
    locateCachedProperty(jit, R0, T11, T12);
    jit.load64(CCallHelpers::BaseIndex(T12, T11, CCallHelpers::TimesEight), R0);
    jit.ret();

    isNotInObject.link(&jit);
    Jump isInherited = branchIfSlotHas<Slot::pointerIsCell>(jit, T11);
    static_assert(Instance::keyIsCompiledField == (Slot::isGetter | Slot::isIndirect));
    generally.append(branchUnlessSlotHas<Slot::isIndirect>(jit, T11));
    static_assert(Instance::keyIsLookedUp == (Instance::keyIsCompiledField | Slot::pointerIsNotCell));
    Jump isLookedUp = branchIfSlotHas<Slot::pointerIsNotCell>(jit, T11);
    jit.loadPtr(Address(A2, OBJECT_OFFSETOF(Instance::KeyedRead, holder)), T12);
    readCompiledField(jit, generally);

    isLookedUp.link(&jit);
    static_assert(R0 == GPRInfo::returnValueGPR);
    callLookUp(jit, Entry::operationAOTGetPlainly, T12);
    Jump cannotLookUp = jit.branchTest64(CCallHelpers::Zero, R0);
    jit.ret();
    cannotLookUp.link(&jit);
    jit.move(T12, R0);
    generally.append(jit.jump());

    isInherited.link(&jit);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T12);
    jit.loadPtr(Address(T12, static_cast<unsigned>(Entry::MegamorphicCache) * sizeof(void*)), T12);
    jit.load16(Address(T12, MegamorphicCache::offsetOfEpoch()), T12);
    Jump hasAnotherEpoch = jit.branch32(CCallHelpers::NotEqual, T12, Address(A2, OBJECT_OFFSETOF(Instance::KeyedRead, epoch)));
    auto holds = jit.label();
    jit.loadPtr(Address(A2, OBJECT_OFFSETOF(Instance::KeyedRead, holder)), T13);
    Jump isNowhere = jit.branchTestPtr(CCallHelpers::Zero, T13);
    locateCachedProperty(jit, T13, T11, T12);
    jit.load64(CCallHelpers::BaseIndex(T12, T11, CCallHelpers::TimesEight), R0);
    jit.ret();
    hasAnotherEpoch.link(&jit);
    jit.branch32(CCallHelpers::Equal, Address(A2, OBJECT_OFFSETOF(Instance::KeyedRead, epoch)), TrustedImm32(MegamorphicCache::permanentEpoch)).linkTo(holds, &jit);
    jit.or32(TrustedImm32(Instance::epochIsOfDictionary), T12);
    miss.append(jit.branch32(CCallHelpers::NotEqual, T12, Address(A2, OBJECT_OFFSETOF(Instance::KeyedRead, epoch))));
    jit.load32(Address(R0, JSCell::structureIDOffset()), T12);
    structureWithID(jit, T12);
    jit.loadPtr(Address(T12, Structure::propertyTableUnsafeOffset()), T12);
    miss.append(jit.branchTestPtr(CCallHelpers::Zero, T12));
    jit.load32(Address(T12, PropertyTable::offsetOfKeyCount()), T12);
    miss.append(jit.branchPtr(CCallHelpers::NotEqual, Address(A2, OBJECT_OFFSETOF(Instance::KeyedRead, holder)), T12));
    isNowhere.link(&jit);
    jit.move(CCallHelpers::TrustedImm64(JSValue::ValueUndefined), R0);
    jit.ret();

    generally.linkTo(stubLabels()[static_cast<unsigned>(Stub::GetByVal)], &jit);

    miss.link(&jit);
    {
#if CPU(X86_64)
        constexpr GPRReg cache = T10;
        constexpr GPRReg result = T9;
#else
        constexpr GPRReg cache = GPRInfo::argumentGPR7;
        constexpr GPRReg result = GPRInfo::argumentGPR6;
#endif
        static_assert(noOverlap(R0, A1, A2, cache, result, T11, T12, T13));
        CCallHelpers::JumpList isLeftToOperation;
        isLeftToOperation.append(jit.branchTest32(CCallHelpers::Zero, Address(A2, OBJECT_OFFSETOF(Slot, structureID))));
        isLeftToOperation.append(jit.branchIfNotString(A1));
        jit.loadPtr(Address(A1, JSString::offsetOfValue()), A2);
        isLeftToOperation.append(jit.branchIfRopeStringImpl(A2));
        isLeftToOperation.append(jit.branchTest32(CCallHelpers::Zero, Address(A2, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIsAtom())));
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), cache);
        jit.loadPtr(Address(cache, static_cast<unsigned>(Entry::MegamorphicCache) * sizeof(void*)), cache);
        isLeftToOperation.append(jit.branchTestPtr(CCallHelpers::Zero, cache));
        isLeftToOperation.append(jit.loadMegamorphicProperty(CCallHelpers::MegamorphicCacheLocation(cache), R0, A2, nullptr, result, T11, T12, T13));
        jit.move(result, R0);
        jit.ret();
        isLeftToOperation.link(&jit);
    }
    callBinaryOperation(jit, Entry::operationAOTGetByValAndCache);
}

static void callOperationOnThree(CCallHelpers& jit, Entry operation)
{
    loadInstance(jit, T11);
    jit.move(A2, A3);
    jit.move(A1, A2);
    jit.move(R0, A1);
    jit.move(T11, A0);
    jit.loadPtr(Address(T11, Instance::offsetOfRuntimeTable()), T11);
    jit.loadPtr(Address(T11, static_cast<unsigned>(operation) * sizeof(void*)), T11);
    callAndCheckException(jit, T11, Returns::Void);
}

// `A1 in R0`: 1 or 0. See Instance::keyedHas.
static void generateInByValCached(CCallHelpers& jit)
{
    CCallHelpers::JumpList miss;
    miss.append(jit.branchIfNotCell(A1));
    miss.append(jit.branchIfNotCell(R0));
    CCallHelpers::JumpList isNotForTable;
    findInKeyedTable(jit, Instance::offsetOfKeyedHas(), Instance::numberOfKeyedHas, A2, miss, isNotForTable);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T12);
    jit.loadPtr(Address(T12, static_cast<unsigned>(Entry::MegamorphicCache) * sizeof(void*)), T12);
    jit.load16(Address(T12, MegamorphicCache::offsetOfEpoch()), T12);
    jit.urshift64(TrustedImm32(32), T11);
    jit.and32(TrustedImm32((1u << Instance::keyedHasAnswerShift) - 1), T11, T13);
    Jump isOfToday = jit.branch32(CCallHelpers::Equal, T12, T13);
    miss.append(jit.branch32(CCallHelpers::NotEqual, T13, TrustedImm32(MegamorphicCache::permanentEpoch)));
    isOfToday.link(&jit);
    jit.urshift32(TrustedImm32(Instance::keyedHasAnswerShift), T11);
    jit.and32(TrustedImm32(1), T11, R0);
    jit.ret();

    isNotForTable.link(&jit);
    static_assert(Instance::keyedHasIsLookedUp == (Slot::isGetter | Slot::pointerIsNotCell));
    miss.append(jit.branchTest64(CCallHelpers::Zero, T11, CCallHelpers::TrustedImm64(static_cast<int64_t>(Slot::pointerIsNotCell) << 32)));
    callLookUp(jit, Entry::operationAOTHasPlainly, T12);
    Jump cannotLookUp = jit.branch32(CCallHelpers::Equal, R0, TrustedImm32(Instance::cannotLookUp));
    jit.ret();
    cannotLookUp.link(&jit);
    jit.move(T12, R0);

    miss.link(&jit);
    callBinaryOperation(jit, Entry::operationAOTInByValAndCache);
}

// A private name of a class in untyped code is a symbol, and a field an own property under it. The prototype chain plays no part. See Instance::keyedReads.
static void generatePrivateGet(CCallHelpers& jit)
{
    CCallHelpers::JumpList miss;
    miss.append(jit.branchIfNotCell(A1));
    miss.append(jit.branchIfNotCell(R0));
    findInKeyedTable(jit, Instance::offsetOfKeyedReads(), Instance::numberOfKeyedReads, A2, miss, miss, sizeof(Instance::KeyedRead));
    locateCachedProperty(jit, R0, T11, T12);
    jit.load64(CCallHelpers::BaseIndex(T12, T11, CCallHelpers::TimesEight), R0);
    jit.ret();

    miss.link(&jit);
    callBinaryOperation(jit, Entry::operationAOTTypedPrivateGet);
}

static void generatePrivateSet(CCallHelpers& jit)
{
    CCallHelpers::JumpList miss;
    miss.append(jit.branchIfNotCell(A1));
    miss.append(jit.branchIfNotCell(R0));
    findInKeyedTable(jit, Instance::offsetOfKeyedStores(), Instance::numberOfKeyedStores, A3, miss, miss);
    locateCachedProperty(jit, R0, T11, T12);
    jit.store64(A2, CCallHelpers::BaseIndex(T12, T11, CCallHelpers::TimesEight));
    returnAfterStoring(jit, A2);

    miss.link(&jit);
    callOperationOnThree(jit, Entry::operationAOTTypedPrivateSet);
}

static void generatePrivateHas(CCallHelpers& jit)
{
    CCallHelpers::JumpList miss;
    CCallHelpers::JumpList isNoLocation;
    miss.append(jit.branchIfNotCell(A1));
    miss.append(jit.branchIfNotCell(R0));
    findInKeyedTable(jit, Instance::offsetOfKeyedReads(), Instance::numberOfKeyedReads, A2, miss, isNoLocation, sizeof(Instance::KeyedRead));
    jit.move(CCallHelpers::TrustedImm64(JSValue::ValueTrue), R0);
    jit.ret();

    isNoLocation.link(&jit);
    static_assert(Instance::keyIsAbsent == (Slot::isGetter | Slot::pointerIsNotCell));
    miss.append(branchUnlessSlotHas<Slot::pointerIsNotCell>(jit, T11));
    jit.move(CCallHelpers::TrustedImm64(JSValue::ValueFalse), R0);
    jit.ret();

    miss.link(&jit);
    callBinaryOperation(jit, Entry::operationAOTTypedPrivateHas);
}

static void generatePrivateFieldAdd(CCallHelpers& jit)
{
    CCallHelpers::JumpList miss;
    miss.append(jit.branchIfNotCell(A1));
    miss.append(jit.branchIfNotCell(R0));
    findInKeyedTable(jit, Instance::offsetOfKeyedAdditions(), Instance::numberOfKeyedAdditions, A3, miss, miss, sizeof(Instance::KeyedAddition));
    static_assert(!OBJECT_OFFSETOF(Instance::KeyedAddition, slot));
    jit.load32(Address(A3, OBJECT_OFFSETOF(Instance::KeyedAddition, newStructureID)), T13);
    locateCachedProperty(jit, R0, T11, T12);
    jit.store64(A2, CCallHelpers::BaseIndex(T12, T11, CCallHelpers::TimesEight));
    jit.store32(T13, Address(R0, JSCell::structureIDOffset()));
    returnAfterStoring(jit, R0);

    miss.link(&jit);
    callOperationOnThree(jit, Entry::operationAOTTypedPrivateFieldAdd);
}

static void generatePutByValCached(CCallHelpers& jit, uint32_t isStrict)
{
    CCallHelpers::JumpList miss;
    CCallHelpers::JumpList generally;
    generally.append(jit.branchIfNotCell(A1));
    generally.append(jit.branchIfNotCell(R0));
    findInKeyedTable(jit, Instance::offsetOfKeyedStores(), Instance::numberOfKeyedStores, A3, miss, generally);
    locateCachedProperty(jit, R0, T11, T12);
    jit.store64(A2, CCallHelpers::BaseIndex(T12, T11, CCallHelpers::TimesEight));
    returnAfterStoring(jit, A2);

    generally.link(&jit);
    jit.move(TrustedImm32(isStrict), A3);
    jit.jump().linkTo(stubLabels()[static_cast<unsigned>(Stub::PutByVal)], &jit);

    // The object lacks the property, and a store under the key has added it to such an object before: see Instance::KeyedAddition.
    miss.link(&jit);
    {
        using KeyedAddition = Instance::KeyedAddition;
        CCallHelpers::JumpList isNotKnown;
        static_assert(!OBJECT_OFFSETOF(KeyedAddition, slot));
        findInKeyedTable(jit, Instance::offsetOfKeyedAdditions(), Instance::numberOfKeyedAdditions, A3, isNotKnown, isNotKnown, sizeof(KeyedAddition));
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T12);
        jit.loadPtr(Address(T12, static_cast<unsigned>(Entry::MegamorphicCache) * sizeof(void*)), T12);
        isNotKnown.append(jit.branchTestPtr(CCallHelpers::Zero, T12));
        jit.load16(Address(T12, MegamorphicCache::offsetOfEpoch()), T12);
        isNotKnown.append(jit.branch32(CCallHelpers::NotEqual, T12, Address(A3, OBJECT_OFFSETOF(KeyedAddition, epoch))));
        jit.load32(Address(A3, OBJECT_OFFSETOF(KeyedAddition, newStructureID)), T13);
        Jump allocates = jit.branchTest32(CCallHelpers::NonZero, Address(A3, OBJECT_OFFSETOF(KeyedAddition, allocatesInitialStorage)));
        locateCachedProperty(jit, R0, T11, T12);
        jit.store64(A2, CCallHelpers::BaseIndex(T12, T11, CCallHelpers::TimesEight));
        jit.store32(T13, Address(R0, JSCell::structureIDOffset()));
        CCallHelpers::Label added = jit.label();
        returnAfterStoring(jit, A2);

        allocates.link(&jit);
        constexpr size_t storageSize = initialOutOfLineCapacity * sizeof(EncodedJSValue);
        constexpr GPRReg allocator = T10;
        constexpr GPRReg storage = T11;
        isNotKnown.append(jit.branchTestPtr(CCallHelpers::NonZero, Address(R0, JSObject::butterflyOffset())));
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfAuxiliarySpace()), allocator);
        jit.loadPtr(Address(allocator, CompleteSubspace::offsetOfAllocatorForSizeStep() + MarkedSpace::sizeClassToIndex(storageSize) * sizeof(Allocator)), allocator);
        isNotKnown.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
        jit.emitAllocateWithNonNullAllocator(storage, JITAllocator::variable(), allocator, T12, isNotKnown, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
        for (unsigned i = 0; i + 1 < initialOutOfLineCapacity; ++i)
            jit.store64(TrustedImm32(0), Address(storage, i * sizeof(EncodedJSValue)));
        jit.store64(A2, Address(storage, storageSize - sizeof(EncodedJSValue)));
        jit.addPtr(TrustedImm32(storageSize + sizeof(IndexingHeader)), storage);
        jit.or32(TrustedImm32(StructureID::nukedStructureIDBit), Address(R0, JSCell::structureIDOffset()));
        jit.storeFence();
        jit.storePtr(storage, Address(R0, JSObject::butterflyOffset()));
        jit.storeFence();
        jit.store32(T13, Address(R0, JSCell::structureIDOffset()));
        jit.jump().linkTo(added, &jit);

        isNotKnown.link(&jit);
    }
    callOperationOnThree(jit, isStrict ? Entry::operationAOTPutByValAndCache : Entry::operationAOTPutByValAndCacheSloppy);
}
static void generatePutByValCached(CCallHelpers& jit) { generatePutByValCached(jit, 1); }
static void generatePutByValCachedSloppy(CCallHelpers& jit) { generatePutByValCached(jit, 0); }

// A1: an ordinary object. A2: the second slot of a property run: see cachePropertyRunWithStorage(). A3: the values, one behind the other. T9: how many. Yields 1: the object has them all and is of the structure
// behind them. 0: nothing is done.
static void generatePutPropertiesWithStorage(CCallHelpers& jit)
{
    using BaseIndex = CCallHelpers::BaseIndex;
    constexpr GPRReg object = A1;
    constexpr GPRReg slot = A2;
    constexpr GPRReg values = A3;
    constexpr GPRReg count = T9;
    constexpr GPRReg storage = R0;
    static_assert(noOverlap(object, slot, values, count, storage, T10, T11, T12, T13));
    static_assert(MarkedSpace::sizeStep == 2 * sizeof(EncodedJSValue) && sizeof(Allocator) == sizeof(void*));
    CCallHelpers::JumpList isNotDone;
    jit.load64(slotWord(slot, 0), T11);
    jit.load32(Address(object, JSCell::structureIDOffset()), T12);
    isNotDone.append(jit.branch32(CCallHelpers::NotEqual, T11, T12));
    jit.load32(Address(slot, OBJECT_OFFSETOF(Slot, fieldType)), T13);
    jit.move(TrustedImm32(0), storage);
    Jump hasStorage = jit.branchTest32(CCallHelpers::Zero, T13, TrustedImm32(PropertyRunStorage::isAllocated));
    jit.and32(TrustedImm32(PropertyRunStorage::capacityMask), T13, T12);
    jit.add32(TrustedImm32(1), T12);
    jit.urshift32(TrustedImm32(1), T12);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfAuxiliarySpace()), T10);
    jit.loadPtr(BaseIndex(T10, T12, CCallHelpers::TimesEight, CompleteSubspace::offsetOfAllocatorForSizeStep()), T12);
    isNotDone.append(jit.branchTestPtr(CCallHelpers::Zero, T12));
    jit.emitAllocateWithNonNullAllocator(storage, JITAllocator::variable(), T12, T10, isNotDone, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
    hasStorage.link(&jit);

    // Those that go into the object.
    jit.urshift64(TrustedImm32(32), T11);
    jit.and32(TrustedImm32(Slot::directLocationMask), T11, T12);
    jit.getEffectiveAddress(BaseIndex(object, T12, CCallHelpers::TimesEight), T12);
    jit.urshift32(TrustedImm32(Slot::nameIDShift), T11);
    jit.and32(TrustedImm32(0xffff), T11);
    jit.sub32(T11, count);
    auto nextInObject = jit.label();
    Jump isFull = jit.branchTest32(CCallHelpers::Zero, T11);
    jit.load64(Address(values), T10);
    jit.store64(T10, Address(T12));
    jit.addPtr(TrustedImm32(sizeof(EncodedJSValue)), values);
    jit.addPtr(TrustedImm32(sizeof(EncodedJSValue)), T12);
    jit.sub32(TrustedImm32(1), T11);
    jit.jump().linkTo(nextInObject, &jit);
    isFull.link(&jit);

    // The rest, downwards from the butterfly, behind what is there. T12: the place of the next.
    constexpr int32_t firstBelowButterfly = sizeof(IndexingHeader) + sizeof(EncodedJSValue);
    auto loadFreshButterfly = [&] {
        jit.and32(TrustedImm32(PropertyRunStorage::capacityMask), T13, T12);
        jit.getEffectiveAddress(BaseIndex(storage, T12, CCallHelpers::TimesEight, sizeof(IndexingHeader)), T12);
    };
    jit.urshift32(T13, TrustedImm32(PropertyRunStorage::firstShift), T11);
    Jump isFresh = jit.branchTestPtr(CCallHelpers::NonZero, storage);
    jit.loadPtr(Address(object, JSObject::butterflyOffset()), T12);
    jit.lshift32(TrustedImm32(3), T11);
    jit.subPtr(T11, T12);
    jit.subPtr(TrustedImm32(firstBelowButterfly), T12);
    Jump isAtNext = jit.jump();
    isFresh.link(&jit);
    loadFreshButterfly();
    jit.subPtr(TrustedImm32(firstBelowButterfly), T12);
    jit.loadPtr(Address(object, JSObject::butterflyOffset()), T13);
    jit.subPtr(TrustedImm32(firstBelowButterfly), T13);
    auto nextToCopy = jit.label();
    Jump isCopied = jit.branchTest32(CCallHelpers::Zero, T11);
    jit.load64(Address(T13), T10);
    jit.store64(T10, Address(T12));
    jit.subPtr(TrustedImm32(sizeof(EncodedJSValue)), T13);
    jit.subPtr(TrustedImm32(sizeof(EncodedJSValue)), T12);
    jit.sub32(TrustedImm32(1), T11);
    jit.jump().linkTo(nextToCopy, &jit);
    isCopied.link(&jit);
    jit.load32(Address(slot, OBJECT_OFFSETOF(Slot, fieldType)), T13);
    isAtNext.link(&jit);
    auto nextInStorage = jit.label();
    Jump areStored = jit.branchTest32(CCallHelpers::Zero, count);
    jit.load64(Address(values), T10);
    jit.store64(T10, Address(T12));
    jit.addPtr(TrustedImm32(sizeof(EncodedJSValue)), values);
    jit.subPtr(TrustedImm32(sizeof(EncodedJSValue)), T12);
    jit.sub32(TrustedImm32(1), count);
    jit.jump().linkTo(nextInStorage, &jit);
    areStored.link(&jit);
    jit.load32(Address(slot, OBJECT_OFFSETOF(Slot, newStructureID)), T11);
    Jump keepsStorage = jit.branchTestPtr(CCallHelpers::Zero, storage);
    auto nextToClear = jit.label();
    Jump isCleared = jit.branchPtr(CCallHelpers::Below, T12, storage);
    jit.store64(TrustedImm32(0), Address(T12));
    jit.subPtr(TrustedImm32(sizeof(EncodedJSValue)), T12);
    jit.jump().linkTo(nextToClear, &jit);
    isCleared.link(&jit);
    loadFreshButterfly();
    // As JSObject::nukeStructureAndSetButterfly().
    jit.or32(TrustedImm32(StructureID::nukedStructureIDBit), Address(object, JSCell::structureIDOffset()));
    jit.storeFence();
    jit.storePtr(T12, Address(object, JSObject::butterflyOffset()));
    jit.storeFence();
    keepsStorage.link(&jit);
    jit.store32(T11, Address(object, JSCell::structureIDOffset()));
    jit.move(TrustedImm32(1), R0);
    jit.ret();

    isNotDone.link(&jit);
    jit.move(TrustedImm32(0), R0);
    jit.ret();
}

static void loadStructureIDAfterTransition(CCallHelpers& jit, GPRReg slot, GPRReg result, CCallHelpers::JumpList& miss)
{
    jit.load64(slotWord(slot, 2), result);
    miss.append(branchIfStructureDiffers(jit, R0, result));
    jit.load32(slotWord(slot, 3), result);
}

// See cacheKeyedDelete().
static void generateDeleteByValCached(CCallHelpers& jit, bool isSloppy)
{
    CCallHelpers::JumpList miss;
    miss.append(jit.branchIfNotCell(A1));
    checkPrivateNameCache(jit, A2, miss);
    loadStructureIDAfterTransition(jit, A2, T13, miss);
    locateCachedProperty(jit, R0, T11, T12);
    jit.store64(TrustedImm32(0), CCallHelpers::BaseIndex(T12, T11, CCallHelpers::TimesEight));
    jit.store32(T13, Address(R0, JSCell::structureIDOffset()));
    if (isSloppy) {
        jit.move(R0, T12);
        jit.move(TrustedImm32(1), R0);
        generateWriteBarrier(jit, T12);
    } else
        returnAfterStoring(jit, R0);

    miss.link(&jit);
    missAtSite(jit, isSloppy ? Entry::operationAOTDeleteByValAndCacheSloppy : Entry::operationAOTDeleteByValAndCache, 2, isSloppy ? Returns::Value : Returns::Void);
}
static void generateDeleteByValCached(CCallHelpers& jit) { generateDeleteByValCached(jit, false); }
static void generateDeleteByValCachedSloppy(CCallHelpers& jit) { generateDeleteByValCached(jit, true); }

// Object.defineProperty() with a literal for a descriptor that has a `value`. R0: the object, if all is well. A1: the value. A2: the slot of the site: see cacheDefinedProperty().
static void generateDefineOwnProperty(CCallHelpers& jit)
{
    CCallHelpers::JumpList miss;
    miss.append(jit.branchIfNotCell(R0));
    jit.load64(slotWord(A2, 0), T11);
    miss.append(branchIfStructureDiffers(jit, R0, T11));
    jit.load64(slotWord(A2, 1), T13);
    locateCachedProperty(jit, R0, T11, T12);
    Jump isThere = jit.branchTest32(CCallHelpers::Zero, T13);
    jit.store64(A1, CCallHelpers::BaseIndex(T12, T11, CCallHelpers::TimesEight));
    jit.store32(T13, Address(R0, JSCell::structureIDOffset()));
    returnAfterStoring(jit, A1);

    // It can be neither changed nor deleted: what it has is all that may be defined.
    isThere.link(&jit);
    jit.load64(CCallHelpers::BaseIndex(T12, T11, CCallHelpers::TimesEight), T13);
    miss.append(jit.branch64(CCallHelpers::NotEqual, T13, A1));
    jit.ret();

    miss.link(&jit);
    missAtSite(jit, Entry::operationAOTTypedDefineOwnPropertyAtSite, 2, Returns::Void);
}

static void generateGetLength(CCallHelpers& jit)
{
    CCallHelpers::JumpList generic;
    generic.append(jit.branchIfNotCell(R0));

    jit.load8(Address(R0, JSCell::indexingTypeAndMiscOffset()), T11);
    Jump notArray = jit.branchTest32(CCallHelpers::Zero, T11, TrustedImm32(IsArray));
    Jump noStorage = jit.branchTest32(CCallHelpers::Zero, T11, TrustedImm32(IndexingShapeMask));
    jit.loadPtr(Address(R0, JSObject::butterflyOffset()), T11);
    jit.load32(Address(T11, Butterfly::offsetOfPublicLength()), T11);
    generic.append(jit.branch32(CCallHelpers::LessThan, T11, TrustedImm32(0)));
    jit.add64(numberTag, T11, R0);
    jit.ret();

    notArray.link(&jit);
    noStorage.link(&jit);
    jit.load8(Address(R0, JSCell::typeInfoTypeOffset()), T11);
    Jump isString = jit.branch32(CCallHelpers::Equal, T11, TrustedImm32(StringType));
    {
        using Proof = Instance::TypedArrayWithBuiltinLength;
        constexpr ptrdiff_t proofs = Instance::offsetOfTypedArraysWithBuiltinLength();
        jit.sub32(TrustedImm32(FirstTypedArrayType), T11);
        generic.append(jit.branch32(CCallHelpers::AboveOrEqual, T11, TrustedImm32(NumberOfTypedArrayTypesExcludingDataView)));
        static_assert(sizeof(Proof) == 32);
        jit.lshiftPtr(TrustedImm32(6), T11);
        jit.addPtr(instanceGPR, T11);
        jit.load32(Address(R0, JSCell::structureIDOffset()), T12);
        Jump isOriginal = jit.branch32(CCallHelpers::Equal, T12, Address(T11, proofs + OBJECT_OFFSETOF(Proof, structureID)));
        jit.addPtr(TrustedImm32(sizeof(Proof)), T11);
        generic.append(jit.branch32(CCallHelpers::NotEqual, T12, Address(T11, proofs + OBJECT_OFFSETOF(Proof, structureID))));
        isOriginal.link(&jit);
        jit.loadPtr(Address(T11, proofs + OBJECT_OFFSETOF(Proof, prototype)), T12);
        jit.load32(Address(T12, JSCell::structureIDOffset()), T12);
        generic.append(jit.branch32(CCallHelpers::NotEqual, T12, Address(T11, proofs + OBJECT_OFFSETOF(Proof, prototypeStructureID))));
        jit.loadPtr(Address(T11, proofs + OBJECT_OFFSETOF(Proof, secondPrototype)), T12);
        jit.load32(Address(T12, JSCell::structureIDOffset()), T12);
        generic.append(jit.branch32(CCallHelpers::NotEqual, T12, Address(T11, proofs + OBJECT_OFFSETOF(Proof, secondPrototypeStructureID))));
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfTypedArrayViewPrototype()), T12);
        jit.load32(Address(T12, JSCell::structureIDOffset()), T11);
        generic.append(jit.branch32(CCallHelpers::NotEqual, T11, Address(instanceGPR, Instance::offsetOfTypedArrayViewPrototypeStructureID())));
        jit.loadPtr(Address(T12, JSObject::butterflyOffset()), T12);
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfTypedArrayLengthOffsetInButterfly()), T11);
        jit.load64(CCallHelpers::BaseIndex(T12, T11, CCallHelpers::TimesOne), T12);
        generic.append(jit.branch64(CCallHelpers::NotEqual, T12, Address(instanceGPR, Instance::offsetOfTypedArrayLengthAccessor())));
        generic.append(jit.branchTest8(CCallHelpers::NonZero, Address(R0, JSArrayBufferView::offsetOfMode()), TrustedImm32(resizabilityAndAutoLengthMask)));
        jit.load64(Address(R0, JSArrayBufferView::offsetOfLength()), T12);
        generic.append(jit.branch64(CCallHelpers::Above, T12, CCallHelpers::TrustedImm64(std::numeric_limits<int32_t>::max())));
        jit.add64(numberTag, T12, R0);
        jit.ret();
    }
    isString.link(&jit);
    jit.loadPtr(Address(R0, JSString::offsetOfValue()), T11);
    Jump isRope = jit.branchIfRopeStringImpl(T11);
    jit.load32(Address(T11, StringImpl::lengthMemoryOffset()), T11);
    jit.add64(numberTag, T11, R0);
    jit.ret();
    isRope.link(&jit);
    jit.load32(Address(R0, JSRopeString::offsetOfLength()), T11);
    jit.add64(numberTag, T11, R0);
    jit.ret();

    generic.link(&jit);
    generateGetByIdWith(jit, Entry::operationAOTGetByIdWellKnown);
}

static void unwind(CCallHelpers& jit, GPRReg vm, GPRReg lookUp)
{
    constexpr GPRReg savedVM = GPRInfo::regCS0;
    RELEASE_ASSERT(noOverlap(vm, lookUp, A0, GPRInfo::regT1));
    jit.emitFunctionPrologue();
    prepareCallOperationWithVM(jit, vm);
    jit.move(vm, GPRInfo::regT1);
    jit.copyCalleeSavesToVMEntryFrameCalleeSavesBuffer(GPRInfo::regT1);
    jit.move(vm, savedVM);
    jit.move(vm, A0);
    jit.call(lookUp, OperationPtrTag);
    constexpr ptrdiff_t offsetOfInstance = 4 * sizeof(Register);
    static_assert(static_cast<int>(CallFrameSlot::callee) < 4);
    jit.subPtr(TrustedImm32(WTF::roundUpToMultipleOf<stackAlignmentBytes()>(offsetOfInstance + Instance::offsetOfVM() + sizeof(void*))), CCallHelpers::stackPointerRegister);
    jit.move(CCallHelpers::stackPointerRegister, GPRInfo::callFrameRegister);
    jit.addPtr(TrustedImm32(offsetOfInstance), GPRInfo::callFrameRegister, GPRInfo::regT1);
    jit.storePtr(GPRInfo::regT1, CCallHelpers::addressFor(CallFrameSlot::codeBlock));
    jit.storePtr(savedVM, Address(GPRInfo::regT1, Instance::offsetOfVM()));
    jit.move(CCallHelpers::TrustedImm64(JSValue::NativeCalleeTag), GPRInfo::regT1);
    jit.store64(GPRInfo::regT1, CCallHelpers::addressFor(CallFrameSlot::callee));
    jit.loadPtr(Address(savedVM, OBJECT_OFFSETOF(VM, targetMachinePCForThrow)), GPRInfo::regT1);
    jit.farJump(GPRInfo::regT1, ExceptionHandlerPtrTag);
}

static void generateHandleException(CCallHelpers& jit)
{
    loadInstance(jit, T9);
    jit.loadPtr(Address(T9, Instance::offsetOfVM()), T10);
    jit.loadPtr(Address(T9, Instance::offsetOfRuntimeTable()), T9);
    jit.loadPtr(Address(T9, static_cast<unsigned>(Entry::LookupExceptionHandler) * sizeof(void*)), T11);
    unwind(jit, T10, T11);
}

static void throwStackOverflow(CCallHelpers& jit)
{
    jit.emitFunctionPrologue();
    jit.move(instanceGPR, A0);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T9);
    jit.loadPtr(Address(T9, static_cast<unsigned>(Entry::ThrowStackOverflowError) * sizeof(void*)), T9);
    jit.call(T9, OperationPtrTag);
    jit.emitFunctionEpilogue();
    generateHandleException(jit);
}

static void generatePollAtPrologue(CCallHelpers& jit)
{
#if CPU(X86_64)
    jit.pop(CCallHelpers::s_scratchRegister);
#endif
    Jump doesNotFit = jit.branchPtr(CCallHelpers::Below, CCallHelpers::stackPointerRegister, Address(instanceGPR, Instance::offsetOfSoftStackLimit()));
#if CPU(X86_64)
    jit.push(CCallHelpers::s_scratchRegister);
#endif
    preservingRegistersOfCaller(jit, false, [&](unsigned bytes) {
        loadReturnAddressOfCaller(jit, bytes, A1);
        jit.move(instanceGPR, A0);
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T11);
        jit.loadPtr(Address(T11, static_cast<unsigned>(Entry::operationAOTTypedPollAtPrologue) * sizeof(void*)), T11);
        jit.call(T11, OperationPtrTag);
#if CPU(X86_64)
        jit.storePtr(GPRInfo::returnValueGPR, Address(CCallHelpers::stackPointerRegister, bytes));
#else
        jit.storePtr(GPRInfo::returnValueGPR, Address(CCallHelpers::stackPointerRegister));
#endif
    });
#if CPU(X86_64)
    Jump hasThrown = jit.branchTestPtr(CCallHelpers::Zero, Address(CCallHelpers::stackPointerRegister));
#else
    Jump hasThrown = jit.branchTestPtr(CCallHelpers::Zero, CCallHelpers::linkRegister);
#endif
    jit.ret();
    hasThrown.link(&jit);
    s_callsBetweenStubs->append({ jit.nearTailCall(), Stub::HandleExceptionAtPrologue });
    doesNotFit.link(&jit);
    s_callsBetweenStubs->append({ jit.nearTailCall(), Stub::ThrowStackOverflowAtPrologue });
}

static void generateHandleExceptionAtPrologue(CCallHelpers& jit)
{
    jit.emitFunctionEpilogue();
    generateHandleException(jit);
}

static void generateThrowStackOverflowAtPrologue(CCallHelpers& jit)
{
    jit.emitFunctionEpilogue();
    throwStackOverflow(jit);
}

static void generateThrowStackOverflow(CCallHelpers& jit)
{
    throwStackOverflow(jit);
}

static void generateCatch(CCallHelpers& jit)
{
    constexpr GPRReg vm = GPRInfo::regT3;
    jit.load64(CCallHelpers::addressFor(CallFrameSlot::callee), vm);
#if ENABLE(WEBASSEMBLY)
    jit.and64(CCallHelpers::TrustedImm64(JSValue::NativeCalleeMask), vm, GPRInfo::regT0);
    Jump isJSCallee = jit.branch64(CCallHelpers::NotEqual, GPRInfo::regT0, CCallHelpers::TrustedImm64(JSValue::NativeCalleeTag));
    jit.loadPtr(CCallHelpers::addressFor(CallFrameSlot::codeBlock), vm);
    jit.loadPtr(Address(vm, JSWebAssemblyInstance::offsetOfVM()), vm);
    Jump vmReady = jit.jump();
    isJSCallee.link(&jit);
#endif
    Jump isPreciseAllocation = jit.branchTestPtr(CCallHelpers::NonZero, vm, TrustedImm32(PreciseAllocation::halfAlignment));
    jit.andPtr(CCallHelpers::TrustedImmPtr(MarkedBlock::blockMask), vm);
    jit.loadPtr(Address(vm, MarkedBlock::offsetOfHeader + MarkedBlock::Header::offsetOfVM()), vm);
    Jump alsoLoadsVM = jit.jump();
    isPreciseAllocation.link(&jit);
    jit.loadPtr(Address(vm, PreciseAllocation::offsetOfWeakSet() + WeakSet::offsetOfVM() - PreciseAllocation::headerSize()), vm);
    alsoLoadsVM.link(&jit);
#if ENABLE(WEBASSEMBLY)
    vmReady.link(&jit);
#endif

    jit.restoreCalleeSavesFromVMEntryFrameCalleeSavesBuffer(vm, GPRInfo::regT0);
    jit.loadPtr(Address(vm, VM::callFrameForCatchOffset()), GPRInfo::callFrameRegister);
    jit.storePtr(CCallHelpers::TrustedImmPtr(nullptr), Address(vm, VM::callFrameForCatchOffset()));
    jit.loadPtr(Address(vm, OBJECT_OFFSETOF(VM, targetMachinePCAfterCatch)), GPRInfo::regT1);
    jit.farJump(GPRInfo::regT1, ExceptionHandlerPtrTag);
}

// Behind Catch, for a frame that returns a rejected promise in place of what is thrown in it: see UnwindFunctor::rejectsInFrame(). The callee saves are what they were when the frame was entered. The stack pointer stays where
// the exception was thrown, below the frame and all it called: whoever walks the stack from the frame while the promise is rejected finds it as it was.
static void generateRejectInFrame(CCallHelpers& jit)
{
    jit.move(CCallHelpers::stackPointerRegister, T9);
    jit.andPtr(TrustedImm32(-static_cast<int32_t>(stackAlignmentBytes())), T9);
    jit.move(T9, CCallHelpers::stackPointerRegister);
    jit.move(instanceGPR, A0);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T9);
    jit.loadPtr(Address(T9, static_cast<unsigned>(Entry::operationAOTTypedRejectWithException) * sizeof(void*)), T9);
    jit.call(T9, OperationPtrTag);
    jit.emitFunctionEpilogue();
    jit.ret();
}

enum class AdapterPasses : uint8_t {
    Arguments,
    // To the function that runs the body of a coroutine: the environment of the callee, the first argument as an integer, the second.
    FrameAndResumeMode,
};

static void adapt(CCallHelpers& jit, GPRReg word, GPRReg instance, AdapterPasses passes = AdapterPasses::Arguments)
{
    ASSERT(noOverlap(word, instance, thisGPR, countGPR, calleeGPR));
    jit.emitFunctionPrologue();
    static_assert(!(adapterSaveAreaSize % stackAlignmentBytes()));
    jit.subPtr(TrustedImm32(adapterSaveAreaSize), CCallHelpers::stackPointerRegister);
#if CPU(ARM64)
    static_assert(offsetOfCellCageBaseRegisterInAdapter == offsetOfInstanceInAdapter + 8 && offsetOfNotCellMaskRegisterInAdapter == offsetOfNumberTagRegisterInAdapter + 8);
    jit.storePair64(instance, cellCageBaseGPR, GPRInfo::callFrameRegister, TrustedImm32(offsetOfInstanceInAdapter));
    jit.storePtr(instanceGPR, Address(GPRInfo::callFrameRegister, offsetOfInstanceRegisterInAdapter));
    jit.storePair64(GPRInfo::numberTagRegister, GPRInfo::notCellMaskRegister, GPRInfo::callFrameRegister, TrustedImm32(offsetOfNumberTagRegisterInAdapter));
#else
    jit.storePtr(instanceGPR, Address(GPRInfo::callFrameRegister, offsetOfInstanceRegisterInAdapter));
    jit.storePtr(GPRInfo::numberTagRegister, Address(GPRInfo::callFrameRegister, offsetOfNumberTagRegisterInAdapter));
    jit.storePtr(GPRInfo::notCellMaskRegister, Address(GPRInfo::callFrameRegister, offsetOfNotCellMaskRegisterInAdapter));
    jit.storePtr(X86Registers::ebx, Address(GPRInfo::callFrameRegister, offsetOfRBXInAdapter));
    jit.storePtr(X86Registers::r12, Address(GPRInfo::callFrameRegister, offsetOfR12InAdapter));
    jit.storePtr(instance, Address(GPRInfo::callFrameRegister, offsetOfInstanceInAdapter));
#endif
    jit.move(instance, instanceGPR);
#if CPU(ARM64)
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfCellCageBase()), cellCageBaseGPR);
#endif
    jit.emitMaterializeTagCheckRegisters();
    jit.storePtr(CCallHelpers::TrustedImmPtr(nullptr), CCallHelpers::addressFor(CallFrameSlot::codeBlock));

    jit.load64(CCallHelpers::addressFor(CallFrameSlot::callee), calleeGPR);
    if (passes == AdapterPasses::Arguments) {
#if CPU(ARM64)
        static_assert(static_cast<int>(CallFrameSlot::thisArgument) == static_cast<int>(CallFrameSlot::argumentCountIncludingThis) + 1);
        jit.loadPair64(GPRInfo::callFrameRegister, TrustedImm32(static_cast<int>(CallFrameSlot::argumentCountIncludingThis) * static_cast<int>(sizeof(Register))), countGPR, thisGPR);
#else
        jit.load64(CCallHelpers::addressFor(CallFrameSlot::thisArgument), thisGPR);
        jit.load32(CCallHelpers::lowWordFor(CallFrameSlot::argumentCountIncludingThis), countGPR);
#endif
        jit.sub32(TrustedImm32(1), countGPR);
        Jump isNotCell = jit.branchIfNotCell(thisGPR);
        jit.load8(Address(thisGPR, JSCell::typeInfoTypeOffset()), argumentGPR(0));
        jit.sub32(TrustedImm32(FirstScopeType), argumentGPR(0));
        Jump isNotScope = jit.branch32(CCallHelpers::Above, argumentGPR(0), TrustedImm32(LastScopeType - FirstScopeType));
        jit.move(CCallHelpers::TrustedImm64(JSValue::ValueUndefined), thisGPR);
        isNotScope.link(&jit);
        isNotCell.link(&jit);
    }
    CCallHelpers::JumpList takesList;
    CCallHelpers::JumpList passesFewer;
    if (passes == AdapterPasses::FrameAndResumeMode) {
        jit.loadPtr(Address(calleeGPR, Typed::Closure::offsetOfCaptures()), argumentGPR(0));
        jit.load32(CCallHelpers::addressFor(virtualRegisterForArgumentIncludingThis(1)), argumentGPR(1));
        jit.load64(CCallHelpers::addressFor(virtualRegisterForArgumentIncludingThis(2)), argumentGPR(2));
    } else {
        takesList.append(jit.branchTest64(CCallHelpers::NonZero, word, CCallHelpers::TrustedImm64(1LL << EntryWord::isListBit)));
        // Nobody looks at a register behind the last parameter.
        constexpr GPRReg numberOfParameters = argumentGPR(numberOfArgumentGPRs - 1);
        ASSERT(noOverlap(word, numberOfParameters));
        extractBits(jit, word, EntryWord::numberOfParametersShift, EntryWord::numberOfParametersBits, numberOfParameters);
        passesFewer.append(jit.branch32(CCallHelpers::Below, countGPR, numberOfParameters));
#if CPU(ARM64)
        static_assert(!(numberOfArgumentGPRs % 2));
        for (unsigned i = 0; i < numberOfArgumentGPRs; i += 2)
            jit.loadPair64(GPRInfo::callFrameRegister, TrustedImm32(virtualRegisterForArgumentIncludingThis(i + 1).offset() * static_cast<int>(sizeof(Register))), argumentGPR(i), argumentGPR(i + 1));
#else
        for (unsigned i = 0; i < numberOfArgumentGPRs; ++i)
            jit.load64(CCallHelpers::addressFor(virtualRegisterForArgumentIncludingThis(i + 1)), argumentGPR(i));
#endif
    }
    CCallHelpers::Label ready = jit.label();
    jit.and64(CCallHelpers::TrustedImm64(static_cast<int64_t>(EntryWord::addressMask)), word);
    jit.call(word, JSEntryPtrTag);
    s_returnsIntoAdapters->append(jit.label());

    jit.loadPtr(Address(GPRInfo::callFrameRegister, offsetOfInstanceRegisterInAdapter), instanceGPR);
#if CPU(ARM64)
    jit.loadPtr(Address(GPRInfo::callFrameRegister, offsetOfCellCageBaseRegisterInAdapter), cellCageBaseGPR);
    jit.loadPair64(GPRInfo::callFrameRegister, TrustedImm32(offsetOfNumberTagRegisterInAdapter), GPRInfo::numberTagRegister, GPRInfo::notCellMaskRegister);
#else
    jit.loadPtr(Address(GPRInfo::callFrameRegister, offsetOfNumberTagRegisterInAdapter), GPRInfo::numberTagRegister);
    jit.loadPtr(Address(GPRInfo::callFrameRegister, offsetOfNotCellMaskRegisterInAdapter), GPRInfo::notCellMaskRegister);
    jit.loadPtr(Address(GPRInfo::callFrameRegister, offsetOfRBXInAdapter), X86Registers::ebx);
    jit.loadPtr(Address(GPRInfo::callFrameRegister, offsetOfR12InAdapter), X86Registers::r12);
#endif
    jit.emitFunctionEpilogue();
    jit.ret();

    if (passes == AdapterPasses::Arguments) {
        passesFewer.link(&jit);
#if CPU(X86_64)
        for (unsigned i = 0; i < numberOfArgumentGPRs; ++i) {
            jit.move(CCallHelpers::TrustedImm64(JSValue::ValueUndefined), argumentGPR(i));
            Jump isNotPassed = jit.branch32(CCallHelpers::BelowOrEqual, countGPR, TrustedImm32(i));
            jit.load64(CCallHelpers::addressFor(virtualRegisterForArgumentIncludingThis(i + 1)), argumentGPR(i));
            isNotPassed.link(&jit);
        }
#else
        ASSERT(noOverlap(word, T13));
        jit.move(CCallHelpers::TrustedImm64(JSValue::ValueUndefined), T13);
        for (unsigned i = 0; i < numberOfArgumentGPRs; ++i) {
            jit.load64(CCallHelpers::addressFor(virtualRegisterForArgumentIncludingThis(i + 1)), argumentGPR(i));
            jit.moveConditionally32(CCallHelpers::Above, countGPR, TrustedImm32(i), argumentGPR(i), T13, argumentGPR(i));
        }
#endif
        jit.jump().linkTo(ready, &jit);
        takesList.link(&jit);
        jit.move(countGPR, argumentGPR(0));
        jit.addPtr(TrustedImm32(virtualRegisterForArgumentIncludingThis(1).offset() * static_cast<int>(sizeof(Register))), GPRInfo::callFrameRegister, argumentGPR(1));
        jit.jump().linkTo(ready, &jit);
    }
}

static void generateEnterModule(CCallHelpers& jit)
{
    constexpr ptrdiff_t afterArguments = static_cast<ptrdiff_t>(AbstractModuleRecord::Argument::NumberOfArguments) * sizeof(Register);
    jit.loadPtr(incomingFrameSlot(CallFrameSlot::firstArgument, afterArguments), entryT12);
    jit.loadPtr(incomingFrameSlot(CallFrameSlot::firstArgument, afterArguments + sizeof(Register)), T11);
    adapt(jit, T11, entryT12);
}

static void generateEnter(CCallHelpers& jit)
{
    jit.loadPtr(incomingFrameSlot(CallFrameSlot::codeBlock), T11);
    jit.loadPtr(Address(T11, CodeBlock::jitCodeOffset()), T11);
    jit.loadPtr(Address(T11, JITCode::offsetOfInstance()), entryT12);
    jit.loadPtr(Address(T11, JITCode::offsetOfEntry()), T11);
    adapt(jit, T11, entryT12);
}

static void generateEnterFunction(CCallHelpers& jit, CodeSpecializationKind kind)
{
    jit.loadPtr(incomingFrameSlot(CallFrameSlot::callee), T11);
    jit.loadPtr(Address(T11, JSFunction::offsetOfExecutableOrRareData()), T11);
    Jump hasExecutable = jit.branchTestPtr(CCallHelpers::Zero, T11, TrustedImm32(JSFunction::rareDataTag));
    jit.loadPtr(Address(T11, FunctionRareData::offsetOfExecutable() - JSFunction::rareDataTag), T11);
    hasExecutable.link(&jit);
    jit.loadPtr(Address(T11, ExecutableBase::offsetOfJITCodeFor(kind)), T11);
    jit.loadPtr(Address(T11, JITCode::offsetOfInstance()), entryT12);
    jit.loadPtr(Address(T11, JITCode::offsetOfEntry()), T11);
    adapt(jit, T11, entryT12);
}
static void generateEnterFunctionForCall(CCallHelpers& jit) { generateEnterFunction(jit, CodeSpecializationKind::CodeForCall); }
static void generateEnterFunctionForConstruct(CCallHelpers& jit) { generateEnterFunction(jit, CodeSpecializationKind::CodeForConstruct); }

static void findTargetAndCall(CCallHelpers&);

template<typename LoadCallLinkInfo>
static void dispatchCall(CCallHelpers& jit, CodeSpecializationKind kind, const LoadCallLinkInfo& loadCallLinkInfo)
{
    constexpr GPRReg T12 = entryT12;
    static_assert(noOverlap(BaselineJITRegisters::Call::calleeGPR, BaselineJITRegisters::Call::callLinkInfoGPR, T11, T12));
    CCallHelpers::JumpList slow;
    slow.append(jit.branchIfNotCell(BaselineJITRegisters::Call::calleeGPR, DoNotHaveTagRegisters));
    Jump isNotFunction = jit.branchIfNotFunction(BaselineJITRegisters::Call::calleeGPR);
    jit.loadPtr(Address(BaselineJITRegisters::Call::calleeGPR, JSFunction::offsetOfExecutableOrRareData()), T11);
    Jump hasExecutable = jit.branchTestPtr(CCallHelpers::Zero, T11, TrustedImm32(JSFunction::rareDataTag));
    jit.loadPtr(Address(T11, FunctionRareData::offsetOfExecutable() - JSFunction::rareDataTag), T11);
    hasExecutable.link(&jit);
    jit.loadPtr(Address(T11, ExecutableBase::offsetOfJITCodeWithArityCheckFor(kind)), T12);
    slow.append(jit.branchTestPtr(CCallHelpers::Zero, T12));
    Jump isNative = jit.branchIfNotType(T11, FunctionExecutableType);
    jit.loadPtr(Address(T11, FunctionExecutable::offsetOfCodeBlockFor(kind)), T11);
    jit.storePtr(T11, incomingFrameSlot(CallFrameSlot::codeBlock));
    isNative.link(&jit);
    jit.farJump(T12, JSEntryPtrTag);

    isNotFunction.link(&jit);
    Jump isSlimClosure;
    if (isCall(kind))
        isSlimClosure = jit.branchIfType(BaselineJITRegisters::Call::calleeGPR, slimClosureTypes);
    slow.append(jit.branchIfNotType(BaselineJITRegisters::Call::calleeGPR, InternalFunctionType));
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T12);
    jit.loadPtr(Address(T12, static_cast<unsigned>(isCall(kind) ? Entry::InternalFunctionCallTrampoline : Entry::InternalFunctionConstructTrampoline) * sizeof(void*)), T12);
    jit.farJump(T12, JSEntryPtrTag);

    if (isCall(kind)) {
        isSlimClosure.link(&jit);
        jit.storePtr(CCallHelpers::TrustedImmPtr(nullptr), incomingFrameSlot(CallFrameSlot::codeBlock));
        s_callsBetweenStubs->append({ jit.nearTailCall(), Stub::EnterSlimClosure });
    }

    slow.link(&jit);
    loadCallLinkInfo();
    findTargetAndCall(jit);
}

static void findTargetAndCall(CCallHelpers& jit)
{
    constexpr GPRReg info = BaselineJITRegisters::Call::callLinkInfoGPR;
    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(stackBytesClearedForCallSlowPath), CCallHelpers::stackPointerRegister);
    auto clearTwoRegisters = [&](GPRReg base, size_t offset) {
#if CPU(ARM64)
        jit.storePair64(ARM64Registers::zr, ARM64Registers::zr, base, TrustedImm32(offset));
#else
        jit.store64(TrustedImm32(0), Address(base, offset));
        jit.store64(TrustedImm32(0), Address(base, offset + sizeof(Register)));
#endif
    };
    if constexpr (stackBytesClearedForCallSlowPath <= 256) {
        for (size_t offset = 0; offset < stackBytesClearedForCallSlowPath; offset += 2 * sizeof(Register))
            clearTwoRegisters(CCallHelpers::stackPointerRegister, offset);
    } else {
        static_assert(info != A0 && info != T9);
        jit.move(CCallHelpers::stackPointerRegister, A0);
        jit.addPtr(TrustedImm32(stackBytesClearedForCallSlowPath), A0, T9);
        CCallHelpers::Label loop = jit.label();
        clearTwoRegisters(A0, 0);
        jit.addPtr(TrustedImm32(2 * sizeof(Register)), A0);
        jit.branchPtr(CCallHelpers::Below, A0, T9).linkTo(loop, &jit);
    }
    jit.addPtr(TrustedImm32(stackBytesClearedForCallSlowPath), CCallHelpers::stackPointerRegister);
    jit.subPtr(TrustedImm32(16), CCallHelpers::stackPointerRegister);
    jit.storePtr(info, Address(CCallHelpers::stackPointerRegister));
    jit.move(GPRInfo::callFrameRegister, A0);
    jit.move(info, A1);
    jit.loadPtr(Address(info, VirtualCallInfo::offsetOfFindTarget()), T9);
    jit.call(T9, OperationPtrTag);
    s_returnsFromFindingTargets->append(jit.label());
    jit.loadPtr(Address(CCallHelpers::stackPointerRegister), T11);
    jit.addPtr(TrustedImm32(16), CCallHelpers::stackPointerRegister);
    jit.emitFunctionEpilogue();
    Jump threw = jit.branchTestPtr(CCallHelpers::NonZero, GPRInfo::returnValueGPR2);
    jit.farJump(GPRInfo::returnValueGPR, JSEntryPtrTag);

    threw.link(&jit);
    jit.move(GPRInfo::returnValueGPR2, T10);
    jit.loadPtr(Address(T11, VirtualCallInfo::offsetOfLookupExceptionHandler()), T11);
    unwind(jit, T10, T11);
}

static void loadCalleeFrameCalleeAndVM(CCallHelpers& jit, GPRReg callee, GPRReg vm)
{
    jit.loadPtr(incomingFrameSlot(CallFrameSlot::callee), callee);
    jit.move(callee, vm);
    Jump isPreciseAllocation = jit.branchTestPtr(CCallHelpers::NonZero, vm, TrustedImm32(PreciseAllocation::halfAlignment));
    jit.andPtr(CCallHelpers::TrustedImmPtr(MarkedBlock::blockMask), vm);
    jit.loadPtr(Address(vm, MarkedBlock::offsetOfHeader + MarkedBlock::Header::offsetOfVM()), vm);
    Jump vmReady = jit.jump();
    isPreciseAllocation.link(&jit);
    jit.loadPtr(Address(vm, PreciseAllocation::offsetOfWeakSet() + WeakSet::offsetOfVM() - PreciseAllocation::headerSize()), vm);
    vmReady.link(&jit);
}

static void generateEnterClosure(CCallHelpers& jit)
{
    constexpr GPRReg T12 = entryT12;
    loadCalleeFrameCalleeAndVM(jit, T11, T12);
    jit.load32(Address(T11, JSCell::structureIDOffset()), T9);
    jit.add64(Address(T12, VM::offsetOfStructureIDBase()), T9);
    jit.loadPtr(Address(T9, Structure::offsetOfAOTInstance()), T12);
    loadEntryWordOfClosure(jit, T11, T12, T11, T9);
    adapt(jit, T11, T12);
}

static void loadClosureWordFromButterfly(CCallHelpers& jit, GPRReg storage, GPRReg instance, GPRReg result, GPRReg scratch)
{
    ASSERT(noOverlap(instance, result, scratch));
    jit.load32(Address(storage, Typed::SlimClosure::offsetOfFunctionIndexInButterfly()), result);
    jit.loadPtr(Address(instance, Instance::offsetOfTypedFunctions()), scratch);
    jit.load64(CCallHelpers::BaseIndex(scratch, result, CCallHelpers::TimesEight), result);
}

static void generateEnterSlimClosure(CCallHelpers& jit)
{
    constexpr GPRReg T12 = entryT12;
    loadCalleeFrameCalleeAndVM(jit, T11, T12);
    jit.load32(Address(T11, JSCell::structureIDOffset()), T9);
    jit.add64(Address(T12, VM::offsetOfStructureIDBase()), T9);
    jit.loadPtr(Address(T9, Structure::offsetOfAOTInstance()), T12);
    Jump isAsMade = jit.branchIfType(T11, SlimClosureType);
    jit.loadPtr(Address(T11, Typed::SlimClosure::offsetOfWord()), T11);
    loadClosureWordFromButterfly(jit, T11, T12, T11, T9);
    toEntryWord(jit, T12, T11, T9);
    Jump hasWord = jit.jump();
    isAsMade.link(&jit);
    loadEntryWordOfClosure(jit, T11, T12, T11, T9, Typed::SlimClosure::offsetOfWord());
    hasWord.link(&jit);
    adapt(jit, T11, T12);
}

// The callee is the resume closure of a coroutine, the arguments the resume mode as an int32 and what the coroutine is resumed with.
static void generateEnterResume(CCallHelpers& jit)
{
    constexpr GPRReg T12 = entryT12;
    loadCalleeFrameCalleeAndVM(jit, T11, T12);
    jit.load32(Address(T11, JSCell::structureIDOffset()), T9);
    jit.add64(Address(T12, VM::offsetOfStructureIDBase()), T9);
    jit.loadPtr(Address(T9, Structure::offsetOfAOTInstance()), T12);
    loadEntryWordOfClosure(jit, T11, T12, T11, T9);
    adapt(jit, T11, T12, AdapterPasses::FrameAndResumeMode);
}

static void generateCallBoundFunction(CCallHelpers& jit)
{
    constexpr GPRReg bound = GPRInfo::regT0;
    constexpr GPRReg total = GPRInfo::regT1;
    constexpr GPRReg scratch = GPRInfo::regT2;
    constexpr GPRReg passed = GPRInfo::regT3;
    constexpr GPRReg value = GPRInfo::regT4;
#if CPU(X86_64)
    constexpr GPRReg vm = T9;
#else
    constexpr GPRReg vm = T10;
#endif
    static_assert(noOverlap(bound, total, scratch, passed, value, vm, T11));
    CCallHelpers::JumpList slowCase;

    loadCalleeFrameCalleeAndVM(jit, bound, vm);
    jit.emitFunctionPrologue();
    jit.storePtr(CCallHelpers::TrustedImmPtr(nullptr), CCallHelpers::addressFor(CallFrameSlot::codeBlock));
    jit.store32(TrustedImm32(0), CCallHelpers::highWordFor(CallFrameSlot::argumentCountIncludingThis));

    static_assert(!(sizeof(CallerFrameAndPC) % stackAlignmentBytes()));
    jit.load32(Address(bound, JSBoundFunction::offsetOfBoundArgsLength()), scratch);
    jit.load32(CCallHelpers::lowWordFor(CallFrameSlot::argumentCountIncludingThis), total);
    jit.move(total, passed);
    jit.add32(scratch, total);
    jit.add32(TrustedImm32(CallFrame::headerSizeInRegisters - CallerFrameAndPC::sizeInRegisters), total, scratch);
    jit.lshift32(TrustedImm32(3), scratch);
    jit.add32(TrustedImm32(stackAlignmentBytes() - 1), scratch);
    jit.and32(TrustedImm32(-stackAlignmentBytes()), scratch);
    jit.negPtr(scratch);
    jit.addPtr(CCallHelpers::stackPointerRegister, scratch);
    jit.loadPtr(Address(vm, VM::offsetOfSoftStackLimit()), T11);
    slowCase.append(jit.branchPtr(CCallHelpers::Above, T11, scratch));
    jit.move(scratch, CCallHelpers::stackPointerRegister);

    jit.store32(total, CCallHelpers::calleeFrameLowWordSlot(CallFrameSlot::argumentCountIncludingThis));
    jit.loadValue(Address(bound, JSBoundFunction::offsetOfBoundThis()), value);
    jit.storeValue(value, CCallHelpers::calleeArgumentSlot(0));

    jit.sub32(TrustedImm32(1), passed);
    jit.sub32(TrustedImm32(1), total);
    Jump nonePassed = jit.branchTest32(CCallHelpers::Zero, passed);
    CCallHelpers::Label nextPassed = jit.label();
    jit.sub32(TrustedImm32(1), passed);
    jit.sub32(TrustedImm32(1), total);
    jit.loadValue(CCallHelpers::addressFor(virtualRegisterForArgumentIncludingThis(1)).indexedBy(passed, CCallHelpers::TimesEight), value);
    jit.storeValue(value, CCallHelpers::calleeArgumentSlot(1).indexedBy(total, CCallHelpers::TimesEight));
    jit.branchTest32(CCallHelpers::NonZero, passed).linkTo(nextPassed, &jit);
    nonePassed.link(&jit);

    CCallHelpers::JumpList argumentsReady;
    argumentsReady.append(jit.branchTest32(CCallHelpers::Zero, total));
    Jump fitsInBoundFunction = jit.branch32(CCallHelpers::BelowOrEqual, total, TrustedImm32(JSBoundFunction::maxEmbeddedArgs));
    {
        jit.loadPtr(Address(bound, JSBoundFunction::offsetOfBoundArgs()), passed);
        CCallHelpers::Label next = jit.label();
        jit.sub32(TrustedImm32(1), total);
        jit.loadValue(CCallHelpers::BaseIndex(passed, total, CCallHelpers::TimesEight, JSCellButterfly::offsetOfData()), value);
        jit.storeValue(value, CCallHelpers::calleeArgumentSlot(1).indexedBy(total, CCallHelpers::TimesEight));
        jit.branchTest32(CCallHelpers::NonZero, total).linkTo(next, &jit);
        argumentsReady.append(jit.jump());
    }
    fitsInBoundFunction.link(&jit);
    {
        CCallHelpers::Label next = jit.label();
        jit.sub32(TrustedImm32(1), total);
        jit.loadValue(CCallHelpers::BaseIndex(bound, total, CCallHelpers::TimesEight, JSBoundFunction::offsetOfBoundArgs()), value);
        jit.storeValue(value, CCallHelpers::calleeArgumentSlot(1).indexedBy(total, CCallHelpers::TimesEight));
        jit.branchTest32(CCallHelpers::NonZero, total).linkTo(next, &jit);
    }
    argumentsReady.link(&jit);

    jit.loadPtr(Address(bound, JSBoundFunction::offsetOfTargetFunction()), scratch);
    jit.storeValue(scratch, CCallHelpers::calleeFrameSlot(CallFrameSlot::callee));
    Jump targetIsSlimClosure = jit.branchIfType(scratch, slimClosureTypes);
    jit.loadPtr(Address(scratch, JSFunction::offsetOfExecutableOrRareData()), total);
    Jump hasExecutable = jit.branchTestPtr(CCallHelpers::Zero, total, TrustedImm32(JSFunction::rareDataTag));
    jit.loadPtr(Address(total, FunctionRareData::offsetOfExecutable() - JSFunction::rareDataTag), total);
    hasExecutable.link(&jit);
    jit.loadPtr(Address(total, ExecutableBase::offsetOfJITCodeWithArityCheckFor(CodeSpecializationKind::CodeForCall)), scratch);
    slowCase.append(jit.branchTestPtr(CCallHelpers::Zero, scratch));
    Jump isNative = jit.branchIfNotType(total, FunctionExecutableType);
    jit.loadPtr(Address(total, FunctionExecutable::offsetOfCodeBlockForCall()), passed);
    jit.storePtr(passed, CCallHelpers::calleeFrameCodeBlockBeforeCall());
    isNative.link(&jit);
    jit.call(scratch, JSEntryPtrTag);
    jit.emitFunctionEpilogue();
    jit.ret();

    targetIsSlimClosure.link(&jit);
    callStubFromStub(jit, Stub::EnterSlimClosure);
    jit.emitFunctionEpilogue();
    jit.ret();

    slowCase.link(&jit);
    jit.emitFunctionEpilogue();
    static_assert(!RuntimeTable::offsetOfEntries());
    jit.loadPtr(Address(vm, VM::offsetOfAOTRuntimeTable()), T11);
    jit.loadPtr(Address(T11, static_cast<unsigned>(Entry::NativeCallTrampoline) * sizeof(void*)), T11);
    jit.farJump(T11, JSEntryPtrTag);
}

static void generateVirtualCall(CCallHelpers& jit) { dispatchCall(jit, CodeSpecializationKind::CodeForCall, [] { }); }
static void generateVirtualConstruct(CCallHelpers& jit) { dispatchCall(jit, CodeSpecializationKind::CodeForConstruct, [] { }); }
static void generateVirtualTailCall(CCallHelpers& jit) { dispatchCall(jit, CodeSpecializationKind::CodeForCall, [] { }); }

#if CPU(X86_64)
constexpr GPRReg callTargetGPR = T11;
constexpr GPRReg callTemporaryGPR = T13;
constexpr GPRReg listCallTemporaryGPR = A2;
constexpr GPRReg listTailCallTemporaryGPR = T9;
#else
constexpr GPRReg callTargetGPR = T12;
constexpr GPRReg callTemporaryGPR = T11;
constexpr GPRReg listCallTemporaryGPR = T12;
constexpr GPRReg listTailCallTemporaryGPR = T12;
#endif

static void callWithEngineConvention(CCallHelpers& jit, CodeSpecializationKind kind)
{
    jit.move(calleeGPR, BaselineJITRegisters::Call::calleeGPR);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T11);
    jit.loadPtr(Address(T11, static_cast<unsigned>(kind == CodeSpecializationKind::CodeForCall ? Entry::CallLinkInfoForCall : Entry::CallLinkInfoForConstruct) * sizeof(void*)), BaselineJITRegisters::Call::callLinkInfoGPR);
    callStubFromStub(jit, kind == CodeSpecializationKind::CodeForCall ? Stub::VirtualCall : Stub::VirtualConstruct);
    jit.emitFunctionEpilogue();
    jit.ret();
}

static CCallHelpers::Label& variadicCallStart() { static NeverDestroyed<CCallHelpers::Label> label; return label; }
static CCallHelpers::Label& variadicCallCellLabel() { static NeverDestroyed<CCallHelpers::Label> label; return label; }
static CCallHelpers::Label& callNonClosureLabel(unsigned count) { static NeverDestroyed<std::array<CCallHelpers::Label, numberOfArgumentGPRs + 1>> labels; return labels.get()[count]; }
static CCallHelpers::Label& callNoJSFunctionLabel(unsigned count) { static NeverDestroyed<std::array<CCallHelpers::Label, numberOfArgumentGPRs + 1>> labels; return labels.get()[count]; }
static CCallHelpers::Label& callJSFunctionOfThisInstanceLabel(unsigned count) { static NeverDestroyed<std::array<CCallHelpers::Label, numberOfArgumentGPRs + 1>> labels; return labels.get()[count]; }
static CCallHelpers::Label& callNoClosureLabel(unsigned count) { static NeverDestroyed<std::array<CCallHelpers::Label, numberOfArgumentGPRs + 1>> labels; return labels.get()[count]; }

static std::pair<Jump, Jump> branchIfNotClosureStructure(CCallHelpers& jit, GPRReg structureID, GPRReg scratch)
{
#if CPU(ARM64)
    jit.load64(Address(instanceGPR, Instance::offsetOfTypedClosureStructureIDPair()), scratch);
    Jump isSlim = jit.branch32(CCallHelpers::Equal, structureID, scratch);
    jit.m_assembler.cmp<64>(structureID, scratch, ARM64Assembler::LSR, 32);
    jit.m_assembler.b_cond(ARM64Assembler::ConditionNE);
    AssemblerLabel branch = jit.m_assembler.labelIgnoringWatchpoints();
    jit.m_assembler.nop();
    return { isSlim, Jump(branch, ARM64Assembler::JumpCondition, ARM64Assembler::ConditionNE) };
#else
    UNUSED_PARAM(scratch);
    Jump isSlim = jit.branch32(CCallHelpers::Equal, structureID, Address(instanceGPR, Instance::offsetOfTypedClosureStructureIDPair()));
    return { isSlim, jit.branch32(CCallHelpers::NotEqual, structureID, Address(instanceGPR, Instance::offsetOfTypedClosureStructureIDPair() + sizeof(uint32_t))) };
#endif
}

static void callBoundTarget(CCallHelpers& jit)
{
    CCallHelpers::JumpList isNot;
    isNot.append(jit.branchIfNotCell(calleeGPR));
    jit.load32(Address(calleeGPR, JSCell::structureIDOffset()), T11);
    isNot.append(jit.branch32(CCallHelpers::NotEqual, T11, Address(instanceGPR, Instance::offsetOfBoundFunctionStructureID())));
    jit.load32(Address(calleeGPR, JSBoundFunction::offsetOfBoundArgsLength()), T13);
    Jump hasBoundArguments = jit.branchTest32(CCallHelpers::NonZero, T13);
    jit.loadPtr(Address(calleeGPR, JSBoundFunction::offsetOfTargetFunction()), T11);
    Jump bindsCall = jit.branchPtr(CCallHelpers::Equal, T11, Address(instanceGPR, Instance::offsetOfFunctionPrototypeCall()));
    // `f.bind(thisArg)`
    jit.load64(Address(calleeGPR, JSBoundFunction::offsetOfBoundThis()), thisGPR);
    jit.move(T11, calleeGPR);
    jit.jump().linkTo(variadicCallCellLabel(), &jit);
    // `Function.prototype.call.bind(f)`
    bindsCall.link(&jit);
    jit.load64(Address(calleeGPR, JSBoundFunction::offsetOfBoundThis()), T11);
    isNot.append(jit.branchIfNotCell(T11));
    Jump isFunction = jit.branchIfType(T11, JSFunctionType);
    isNot.append(jit.branchTest8(CCallHelpers::Zero, Address(T11, JSCell::typeInfoFlagsOffset()), TrustedImm32(OverridesGetCallData)));
    isFunction.link(&jit);
    jit.move(T11, calleeGPR);
    Jump passesNothing = jit.branchTest32(CCallHelpers::Zero, countGPR);
    jit.move(argumentGPR(0), thisGPR);
    for (unsigned i = 0; i + 1 < numberOfArgumentGPRs; ++i)
        jit.move(argumentGPR(i + 1), argumentGPR(i));
    jit.sub32(TrustedImm32(1), countGPR);
    jit.jump().linkTo(variadicCallCellLabel(), &jit);
    passesNothing.link(&jit);
    jit.move(CCallHelpers::TrustedImm64(JSValue::ValueUndefined), thisGPR);
    jit.jump().linkTo(variadicCallCellLabel(), &jit);
    // `f.bind(thisArg, a, ..)`: the arguments that are in the cell go in front of what is passed, if there are registers for all.
    hasBoundArguments.link(&jit);
    static_assert(JSBoundFunction::maxEmbeddedArgs < numberOfArgumentGPRs);
    isNot.append(jit.branch32(CCallHelpers::Above, T13, TrustedImm32(JSBoundFunction::maxEmbeddedArgs)));
    jit.add32(countGPR, T13, T11);
    isNot.append(jit.branch32(CCallHelpers::Above, T11, TrustedImm32(numberOfArgumentGPRs)));
    jit.move(T11, countGPR);
    CCallHelpers::JumpList areInPlace;
    for (unsigned bound = 1; bound <= JSBoundFunction::maxEmbeddedArgs; ++bound) {
        Jump hasMore;
        if (bound < JSBoundFunction::maxEmbeddedArgs)
            hasMore = jit.branch32(CCallHelpers::NotEqual, T13, TrustedImm32(bound));
        for (unsigned i = numberOfArgumentGPRs; i-- > bound;)
            jit.move(argumentGPR(i - bound), argumentGPR(i));
        for (unsigned i = 0; i < bound; ++i)
            jit.load64(Address(calleeGPR, JSBoundFunction::offsetOfBoundArgs() + i * sizeof(WriteBarrier<Unknown>)), argumentGPR(i));
        if (bound < JSBoundFunction::maxEmbeddedArgs) {
            areInPlace.append(jit.jump());
            hasMore.link(&jit);
        }
    }
    areInPlace.link(&jit);
    jit.load64(Address(calleeGPR, JSBoundFunction::offsetOfBoundThis()), thisGPR);
    jit.loadPtr(Address(calleeGPR, JSBoundFunction::offsetOfTargetFunction()), calleeGPR);
    jit.jump().linkTo(variadicCallCellLabel(), &jit);
    isNot.link(&jit);
}

static void generateReturnFromCallWithList(CCallHelpers& jit)
{
    callTargetWithList() = jit.label();
    jit.call(callTargetGPR, JSEntryPtrTag);
    returnFromCallWithList() = jit.label();
    jit.emitFunctionEpilogue();
    jit.ret();
}

static void callThroughTrampoline(CCallHelpers& jit, unsigned count, Entry trampoline)
{
    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(WTF::roundUpToMultipleOf<stackAlignmentBytes()>((CallFrame::headerSizeInRegisters - CallerFrameAndPC::sizeInRegisters + 1 + count) * sizeof(Register))), CCallHelpers::stackPointerRegister);
    jit.store64(calleeGPR, outgoingFrameSlot(CallFrameSlot::callee));
    jit.move(TrustedImm32(count + 1), callTemporaryGPR);
    jit.store64(callTemporaryGPR, outgoingFrameSlot(CallFrameSlot::argumentCountIncludingThis));
    jit.store64(thisGPR, outgoingFrameSlot(CallFrameSlot::thisArgument));
    for (unsigned i = 0; i < count; ++i)
        jit.store64(argumentGPR(i), outgoingFrameSlot(CallFrameSlot::thisArgument, (i + 1) * sizeof(Register)));
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T11);
    jit.loadPtr(Address(T11, static_cast<unsigned>(trampoline) * sizeof(void*)), T11);
    jit.call(T11, JSEntryPtrTag);
    jit.emitFunctionEpilogue();
    jit.ret();
}

static void callHostFunction(CCallHelpers& jit, unsigned count)
{
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    auto inHostFrame = [](CallFrameSlot slot, ptrdiff_t offset = 0) {
        return Address(sp, static_cast<int>(slot) * sizeof(Register) + offset);
    };
    static_assert(noOverlap(calleeGPR, T11, A0, A1));
    CCallHelpers::Label start = jit.label();
    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(WTF::roundUpToMultipleOf<stackAlignmentBytes()>((CallFrame::headerSizeInRegisters + 1 + count) * sizeof(Register))), sp);
    jit.storePtr(GPRInfo::callFrameRegister, Address(sp, CallFrame::callerFrameOffset()));
    loadLabelAddress(jit, start, T11);
    jit.storePtr(T11, Address(sp, CallFrame::returnPCOffset()));
    jit.store64(TrustedImm32(0), inHostFrame(CallFrameSlot::codeBlock));
    jit.store64(calleeGPR, inHostFrame(CallFrameSlot::callee));
    jit.store64(TrustedImm32(count + 1), inHostFrame(CallFrameSlot::argumentCountIncludingThis));
    jit.store64(thisGPR, inHostFrame(CallFrameSlot::thisArgument));
    for (unsigned i = 0; i < count; ++i)
        jit.store64(argumentGPR(i), inHostFrame(CallFrameSlot::thisArgument, (i + 1) * sizeof(Register)));
    jit.move(sp, A1);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfVM()), T11);
    jit.storePtr(A1, Address(T11, VM::offsetOfTopCallFrame()));
    jit.loadPtr(Address(calleeGPR, JSCallee::offsetOfScopeChain()), A0);
    jit.loadPtr(Address(calleeGPR, JSFunction::offsetOfExecutableOrRareData()), T11);
    Jump hasRareData = jit.branchTestPtr(CCallHelpers::NonZero, T11, TrustedImm32(JSFunction::rareDataTag));
    CCallHelpers::Label hasExecutable = jit.label();
    jit.call(Address(T11, NativeExecutable::offsetOfNativeFunctionFor(CodeSpecializationKind::CodeForCall)), HostFunctionPtrTag);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfVM()), T11);
    Jump exception = jit.branchTestPtr(CCallHelpers::NonZero, Address(T11, VM::exceptionOffset()));
    jit.emitFunctionEpilogue();
    jit.ret();

    hasRareData.link(&jit);
    jit.loadPtr(Address(T11, FunctionRareData::offsetOfExecutable() - JSFunction::rareDataTag), T11);
    jit.jump().linkTo(hasExecutable, &jit);

    exception.link(&jit);
    jit.emitFunctionEpilogue();
    loadInstance(jit, T9);
    jumpToEntry(jit, T9, Entry::HandleException);
}

static CCallHelpers::Label& callOfTrapLabel() { static NeverDestroyed<CCallHelpers::Label> label; return label; }

static void callTrapOfProxy(CCallHelpers& jit, unsigned count, CCallHelpers::JumpList& isNotPrepared)
{
    static_assert(numberOfArgumentGPRs >= 3);
    RELEASE_ASSERT(callOfTrapLabel().isSet());
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    constexpr unsigned numberOfResults = 5;
    unsigned sizeOfHostFrame = CallFrame::headerSizeInRegisters + 1 + count;
    auto inHostFrame = [](CallFrameSlot slot, ptrdiff_t offset = 0) {
        return Address(sp, static_cast<int>(slot) * sizeof(Register) + offset);
    };
    auto result = [&](unsigned i) {
        return Address(sp, (sizeOfHostFrame + i) * sizeof(Register));
    };
    CCallHelpers::Label start = jit.label();
    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(WTF::roundUpToMultipleOf<stackAlignmentBytes()>((sizeOfHostFrame + numberOfResults + 1) * sizeof(Register))), sp);
    jit.store64(countGPR, result(numberOfResults));
    jit.storePtr(GPRInfo::callFrameRegister, Address(sp, CallFrame::callerFrameOffset()));
    loadLabelAddress(jit, start, T13);
    jit.storePtr(T13, Address(sp, CallFrame::returnPCOffset()));
    jit.store64(TrustedImm32(0), inHostFrame(CallFrameSlot::codeBlock));
    jit.store64(calleeGPR, inHostFrame(CallFrameSlot::callee));
    jit.store64(TrustedImm32(count + 1), inHostFrame(CallFrameSlot::argumentCountIncludingThis));
    jit.store64(thisGPR, inHostFrame(CallFrameSlot::thisArgument));
    for (unsigned i = 0; i < count; ++i)
        jit.store64(argumentGPR(i), inHostFrame(CallFrameSlot::thisArgument, (i + 1) * sizeof(Register)));
    prepareCallOperation(jit, T13);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T9);
    jit.loadPtr(Address(T9, static_cast<unsigned>(Entry::operationAOTTypedPrepareCallOfProxy) * sizeof(void*)), T9);
    jit.move(instanceGPR, A0);
    jit.move(sp, A1);
    jit.move(TrustedImm32(count), A2);
    jit.call(T9, OperationPtrTag);
    Jump isPrepared = jit.branchTestPtr(CCallHelpers::NonZero, GPRInfo::returnValueGPR);
    jit.load64(result(numberOfResults), countGPR);
    jit.load64(inHostFrame(CallFrameSlot::callee), calleeGPR);
    jit.load64(inHostFrame(CallFrameSlot::thisArgument), thisGPR);
    for (unsigned i = 0; i < count; ++i)
        jit.load64(inHostFrame(CallFrameSlot::thisArgument, (i + 1) * sizeof(Register)), argumentGPR(i));
    jit.emitFunctionEpilogue();
    isNotPrepared.append(jit.jump());

    isPrepared.link(&jit);
    jit.load64(result(0), calleeGPR);
    jit.load64(result(1), thisGPR);
    for (unsigned i = 0; i < 3; ++i)
        jit.load64(result(i + 2), argumentGPR(i));
    jit.move(TrustedImm32(3), countGPR);
    jit.move(sp, GPRInfo::callFrameRegister);
    jit.jump().linkTo(callOfTrapLabel(), &jit);
}

// Indirect call of a closure of the typed compiler: the entry point is in the cell. Falls through if the callee is not such a closure of
// this instance, or is a class. With a static count, countGPR is not read. Jumps to `takesList` with the entry word in callTargetGPR if the
// entry point takes its arguments in memory.
static void callTypedClosure(CCallHelpers& jit, std::optional<unsigned> count, CCallHelpers::JumpList& takesList, CCallHelpers::Label* ofCell = nullptr, CCallHelpers::Label* ofNoClosureAsMade = nullptr)
{
    CCallHelpers::JumpList isNotClosure;
    auto enter = [&] {
        Jump hasFlags = jit.branchTest64(CCallHelpers::NonZero, callTargetGPR, CCallHelpers::TrustedImm64(static_cast<int64_t>(1ULL << ClosureWord::isListBit | 1ULL << ClosureWord::isNotCallableBit)));
        if (!count)
            jit.move(CCallHelpers::TrustedImm64(JSValue::ValueUndefined), T13);
        for (unsigned i = count.value_or(0); i < numberOfArgumentGPRs; ++i) {
            if (count)
                jit.move(CCallHelpers::TrustedImm64(JSValue::ValueUndefined), argumentGPR(i));
            else
                jit.moveConditionally32(CCallHelpers::Above, countGPR, TrustedImm32(i), argumentGPR(i), T13, argumentGPR(i));
        }
        toAddressOfCode(jit, callTargetGPR, T13);
        jit.farJump(callTargetGPR, JSEntryPtrTag);
        hasFlags.link(&jit);
        isNotClosure.append(jit.branchTest64(CCallHelpers::NonZero, callTargetGPR, CCallHelpers::TrustedImm64(static_cast<int64_t>(1ULL << ClosureWord::isNotCallableBit))));
        toAddressOfCode(jit, callTargetGPR, T13);
        takesList.append(jit.jump());
    };
    isNotClosure.append(jit.branchIfNotCell(calleeGPR));
    if (ofCell)
        *ofCell = jit.label();
    jit.load32(Address(calleeGPR, JSCell::structureIDOffset()), T11);
    auto [isSlimAsMade, isNeither] = branchIfNotClosureStructure(jit, T11, T13);
    CCallHelpers::Label isFull = jit.label();
    jit.load64(Address(calleeGPR, Typed::Closure::offsetOfEntry()), callTargetGPR);
    enter();
    isSlimAsMade.link(&jit);
    CCallHelpers::Label isSlim = jit.label();
    jit.load64(Address(calleeGPR, Typed::SlimClosure::offsetOfWord()), callTargetGPR);
    CCallHelpers::Label hasSlimClosureWord = jit.label();
    enter();

    CCallHelpers::Label isNotJSFunction = jit.label();
    Jump hasProperties = jit.branch32(CCallHelpers::Equal, T13, TrustedImm32(SlimClosureWithPropertiesType));
    Jump isNotSlimClosure = jit.branch32(CCallHelpers::NotEqual, T13, TrustedImm32(SlimClosureType));
    if (!count)
        isNotClosure.append(isNotSlimClosure);
    structureWithID(jit, T11);
    jit.branchPtr(CCallHelpers::Equal, Address(T11, Structure::offsetOfAOTInstance()), instanceGPR).linkTo(isSlim, &jit);
    isNotClosure.append(jit.jump());
    hasProperties.link(&jit);
    structureWithID(jit, T11);
    isNotClosure.append(jit.branchPtr(CCallHelpers::NotEqual, Address(T11, Structure::offsetOfAOTInstance()), instanceGPR));
    jit.loadPtr(Address(calleeGPR, Typed::SlimClosure::offsetOfWord()), callTargetGPR);
    loadClosureWordFromButterfly(jit, callTargetGPR, instanceGPR, callTargetGPR, T13);
    jit.jump().linkTo(hasSlimClosureWord, &jit);
    if (count) {
        isNotSlimClosure.link(&jit);
        isNotClosure.append(jit.branch32(CCallHelpers::NotEqual, T13, TrustedImm32(ProxyObjectType)));
        callTrapOfProxy(jit, *count, isNotClosure);
    }

    isNeither.link(&jit);
    if (ofNoClosureAsMade)
        *ofNoClosureAsMade = jit.label();
    // An async function or a generator function has another structure, and so has a closure that was given a property (`f.prototype`,
    // `f.displayName = ..`).
    jit.load8(Address(calleeGPR, JSCell::typeInfoTypeOffset()), T13);
    jit.branch32(CCallHelpers::NotEqual, T13, TrustedImm32(JSFunctionType)).linkTo(isNotJSFunction, &jit);
    structureWithID(jit, T11);
    isNotClosure.append(jit.branchPtr(CCallHelpers::NotEqual, Address(T11, Structure::offsetOfAOTInstance()), instanceGPR));
    CCallHelpers::Label isOfThisInstance = jit.label();
    jit.load32(Address(instanceGPR, Instance::offsetOfTypedClosureStructureID(0)), T13);
    structureWithID(jit, T13);
    jit.loadPtr(Address(T13, Structure::classInfoOffset()), T13);
    jit.branchPtr(CCallHelpers::Equal, Address(T11, Structure::classInfoOffset()), T13).linkTo(isFull, &jit);
    isNotClosure.link(&jit);
    if (count && ofNoClosureAsMade) {
        callNoJSFunctionLabel(*count) = isNotJSFunction;
        callJSFunctionOfThisInstanceLabel(*count) = isOfThisInstance;
        callNoClosureLabel(*count) = jit.label();
    }
}

// The callee is a closure of the typed compiler and its ClosureWord is in T11. To the list: [[Call]] of it throws.
static void enterClosure(CCallHelpers& jit, unsigned count, CCallHelpers::JumpList& isNotCallable)
{
    static_assert(ClosureWord::isListBit > ClosureWord::numberOfParametersShift && ClosureWord::isNotCallableBit > ClosureWord::numberOfParametersShift && ClosureWord::constructsOrdinaryObjectBit < ClosureWord::numberOfParametersShift && ClosureWord::functionShift + ClosureWord::numberOfFunctionBits <= ClosureWord::numberOfParametersShift);
    static_assert(noOverlap(T11, T13, calleeGPR, thisGPR, countGPR, argumentGPR(0), argumentGPR(1), argumentGPR(2), argumentGPR(3)));
    // One compare says three things: the flags are above the number of parameters, which is above all else.
    jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(count + 1) << ClosureWord::numberOfParametersShift), T13);
    Jump isNotTheUsual = jit.branch64(CCallHelpers::AboveOrEqual, T11, T13);
    CCallHelpers::Label enter = jit.label();
    toAddressOfCode(jit, T11, T13);
    jit.farJump(T11, JSEntryPtrTag);

    isNotTheUsual.link(&jit);
    isNotCallable.append(jit.branchTest64(CCallHelpers::NonZero, T11, CCallHelpers::TrustedImm64(static_cast<int64_t>(1ULL << ClosureWord::isNotCallableBit))));
    Jump takesList = jit.branchTest64(CCallHelpers::NonZero, T11, CCallHelpers::TrustedImm64(1LL << ClosureWord::isListBit));
    // It has more parameters than are passed.
    for (unsigned i = count; i < numberOfArgumentGPRs; ++i)
        jit.move(CCallHelpers::TrustedImm64(JSValue::ValueUndefined), argumentGPR(i));
    jit.jump().linkTo(enter, &jit);

    // It takes its arguments in memory: a rest parameter, `arguments`. They are all it looks at there.
    takesList.link(&jit);
    jit.emitFunctionPrologue();
    if (count)
        jit.subPtr(TrustedImm32(WTF::roundUpToMultipleOf<stackAlignmentBytes()>(count * sizeof(Register))), CCallHelpers::stackPointerRegister);
    for (unsigned i = 0; i < count; ++i)
        jit.store64(argumentGPR(i), Address(CCallHelpers::stackPointerRegister, i * sizeof(Register)));
    toAddressOfCode(jit, T11, T13);
    jit.move(TrustedImm32(count), argumentGPR(0));
    jit.move(CCallHelpers::stackPointerRegister, argumentGPR(1));
    jit.call(T11, JSEntryPtrTag);
    jit.emitFunctionEpilogue();
    jit.ret();
}

static void generateCallTo(CCallHelpers& jit, CodeSpecializationKind kind, std::optional<unsigned> count)
{
    if (kind == CodeSpecializationKind::CodeForCall && !count)
        variadicCallStart() = jit.label();
    CCallHelpers::JumpList closureTakesList;
    if (kind == CodeSpecializationKind::CodeForCall)
        callTypedClosure(jit, count, closureTakesList, count ? nullptr : &variadicCallCellLabel(), count ? &callNonClosureLabel(*count) : nullptr);
    if (count)
        jit.move(TrustedImm32(*count), countGPR);

    auto spill = [&] {
        unsigned registers = count.value_or(numberOfArgumentGPRs);
        jit.emitFunctionPrologue();
        jit.subPtr(TrustedImm32(WTF::roundUpToMultipleOf<stackAlignmentBytes()>((CallFrame::headerSizeInRegisters - CallerFrameAndPC::sizeInRegisters + 1 + registers) * sizeof(Register))), CCallHelpers::stackPointerRegister);
        jit.store64(calleeGPR, outgoingFrameSlot(CallFrameSlot::callee));
        jit.add32(TrustedImm32(1), countGPR, callTemporaryGPR);
        jit.store64(callTemporaryGPR, outgoingFrameSlot(CallFrameSlot::argumentCountIncludingThis));
        jit.store64(thisGPR, outgoingFrameSlot(CallFrameSlot::thisArgument));
        for (unsigned i = 0; i < registers; ++i)
            jit.store64(argumentGPR(i), outgoingFrameSlot(CallFrameSlot::thisArgument, (i + 1) * sizeof(Register)));
    };
    if (!closureTakesList.empty()) {
        Jump isNoClosure = jit.jump();
        closureTakesList.link(&jit);
        if (count)
            jit.move(TrustedImm32(*count), countGPR);
        loadLabelAddress(jit, returnFromCallWithList(), callTemporaryGPR);
#if CPU(ARM64)
        Jump doesNotReturnToAdapter = jit.branchPtr(CCallHelpers::NotEqual, ARM64Registers::lr, callTemporaryGPR);
#else
        Jump doesNotReturnToAdapter = jit.branchPtr(CCallHelpers::NotEqual, Address(CCallHelpers::stackPointerRegister), callTemporaryGPR);
#endif
        jit.emitFunctionEpilogue();
        doesNotReturnToAdapter.link(&jit);
        spill();
        jit.move(countGPR, argumentGPR(0));
        jit.addPtr(TrustedImm32(outgoingFrameSlot(CallFrameSlot::thisArgument, sizeof(Register)).offset), CCallHelpers::stackPointerRegister, argumentGPR(1));
        jit.and64(CCallHelpers::TrustedImm64(static_cast<int64_t>(EntryWord::addressMask)), callTargetGPR);
        jit.jump().linkTo(callTargetWithList(), &jit);
        isNoClosure.link(&jit);
    }

    if (kind == CodeSpecializationKind::CodeForCall)
        callBoundTarget(jit);
    spill();
    callWithEngineConvention(jit, kind);
    if (kind == CodeSpecializationKind::CodeForCall && !count) {
        callOfTrapLabel() = jit.label();
        callStubFromStub(jit, Stub::Call);
        s_returnsFromFindingTargets->append(jit.label());
        jit.loadPtr(Address(GPRInfo::callFrameRegister, CallFrame::callerFrameOffset()), GPRInfo::callFrameRegister);
        jit.emitFunctionEpilogue();
        jit.ret();
    }
}

static void enterWithArgumentsOfList(CCallHelpers& jit, GPRReg target)
{
    jit.move(argumentGPR(0), countGPR);
    jit.move(argumentGPR(1), T13);
    Jump noMoreArguments[numberOfArgumentGPRs];
    for (unsigned i = 0; i < numberOfArgumentGPRs; ++i) {
        noMoreArguments[i] = jit.branch32(CCallHelpers::BelowOrEqual, countGPR, TrustedImm32(i));
        jit.load64(Address(T13, i * sizeof(Register)), argumentGPR(i));
    }
    jit.farJump(target, JSEntryPtrTag);
    for (unsigned i = 0; i < numberOfArgumentGPRs; ++i) {
        noMoreArguments[i].link(&jit);
        jit.move(CCallHelpers::TrustedImm64(JSValue::ValueUndefined), argumentGPR(i));
    }
    jit.farJump(target, JSEntryPtrTag);
}

static void generateConstructClosureList(CCallHelpers& jit)
{
    static_assert(noOverlap(T11, T13, countGPR, calleeGPR, thisGPR, argumentGPR(0), argumentGPR(1)));
    Jump takesRegisters = jit.branchTest64(CCallHelpers::Zero, T11, CCallHelpers::TrustedImm64(1LL << ClosureWord::isListBit));
    toAddressOfCode(jit, T11, T13);
    jit.farJump(T11, JSEntryPtrTag);
    takesRegisters.link(&jit);
    toAddressOfCode(jit, T11, T13);
    enterWithArgumentsOfList(jit, T11);
}

static void generateCallListTo(CCallHelpers& jit, CodeSpecializationKind kind)
{
    if (kind == CodeSpecializationKind::CodeForCall) {
        // A closure of the typed compiler with the structure it was allocated with, and no class.
        CCallHelpers::JumpList isTypedClosure;
        static_assert(noOverlap(T13, callTargetGPR, calleeGPR, thisGPR, argumentGPR(0), argumentGPR(1)));
        CCallHelpers::JumpList isNot;
        isNot.append(jit.branchIfNotCell(calleeGPR));
        jit.load32(Address(calleeGPR, JSCell::structureIDOffset()), T13);
        auto [isSlim, isNeither] = branchIfNotClosureStructure(jit, T13, callTargetGPR);
        isNot.append(isNeither);
        auto enter = [&] {
            isNot.append(jit.branchTest64(CCallHelpers::NonZero, callTargetGPR, CCallHelpers::TrustedImm64(static_cast<int64_t>(1ULL << ClosureWord::isNotCallableBit))));
            Jump closureTakesRegisters = jit.branchTest64(CCallHelpers::Zero, callTargetGPR, CCallHelpers::TrustedImm64(1LL << ClosureWord::isListBit));
            toAddressOfCode(jit, callTargetGPR, T13);
            jit.farJump(callTargetGPR, JSEntryPtrTag);
            closureTakesRegisters.link(&jit);
            toAddressOfCode(jit, callTargetGPR, T13);
            isTypedClosure.append(jit.jump());
        };
        jit.load64(Address(calleeGPR, Typed::Closure::offsetOfEntry()), callTargetGPR);
        enter();
        isSlim.link(&jit);
        jit.load64(Address(calleeGPR, Typed::SlimClosure::offsetOfWord()), callTargetGPR);
        enter();
        // With the address of its code.
        isTypedClosure.link(&jit);
        enterWithArgumentsOfList(jit, callTargetGPR);
        isNot.link(&jit);
    }

    jit.emitFunctionPrologue();
    jit.move(argumentGPR(0), countGPR);
    jit.move(argumentGPR(1), T13);
    static_assert(stackAlignmentRegisters() == 2);
    jit.add32(TrustedImm32(CallFrame::headerSizeInRegisters - CallerFrameAndPC::sizeInRegisters + 1 + 1), countGPR, T11);
    jit.and32(TrustedImm32(~1), T11);
    jit.lshiftPtr(TrustedImm32(3), T11);
    jit.subPtr(CCallHelpers::stackPointerRegister, T11, T11);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfVM()), listCallTemporaryGPR);
    Jump overflow = jit.branchPtr(CCallHelpers::Above, Address(listCallTemporaryGPR, VM::offsetOfSoftStackLimit()), T11);
    Jump wrapped = jit.branchPtr(CCallHelpers::Above, T11, GPRInfo::callFrameRegister);
    jit.move(T11, CCallHelpers::stackPointerRegister);
    jit.store64(calleeGPR, outgoingFrameSlot(CallFrameSlot::callee));
    jit.add32(TrustedImm32(1), countGPR, T11);
    jit.store64(T11, outgoingFrameSlot(CallFrameSlot::argumentCountIncludingThis));
    jit.store64(thisGPR, outgoingFrameSlot(CallFrameSlot::thisArgument));
    jit.addPtr(TrustedImm32(outgoingFrameSlot(CallFrameSlot::thisArgument, sizeof(Register)).offset), CCallHelpers::stackPointerRegister, T11);
    Jump none = jit.branchTest32(CCallHelpers::Zero, countGPR);
    CCallHelpers::Label next = jit.label();
    jit.sub32(TrustedImm32(1), countGPR);
    jit.load64(CCallHelpers::BaseIndex(T13, countGPR, CCallHelpers::TimesEight), listCallTemporaryGPR);
    jit.store64(listCallTemporaryGPR, CCallHelpers::BaseIndex(T11, countGPR, CCallHelpers::TimesEight));
    jit.branchTest32(CCallHelpers::NonZero, countGPR).linkTo(next, &jit);
    none.link(&jit);
    callWithEngineConvention(jit, kind);

    overflow.link(&jit);
    wrapped.link(&jit);
    jit.emitFunctionEpilogue();
    throwStackOverflow(jit);
}

static void generateCallList(CCallHelpers& jit) { generateCallListTo(jit, CodeSpecializationKind::CodeForCall); }
static void generateCall(CCallHelpers& jit) { generateCallTo(jit, CodeSpecializationKind::CodeForCall, std::nullopt); }
static void generateCallValue(CCallHelpers& jit) { jit.breakpoint(); }
static void generateCallValueWithoutThis(CCallHelpers& jit) { jit.breakpoint(); }
static void generateCallValueNamed(CCallHelpers& jit) { jit.breakpoint(); }
static void generateCallMethod(CCallHelpers& jit) { jit.breakpoint(); }
static void generateCallMethodRead(CCallHelpers& jit) { generateGetByIdWith(jit, Entry::operationAOTGetById, true); }

// calleeGPR, thisGPR, and the list in the first argument register. See Typed::Lowering::lowerApply().
static void generateCallWithArray(CCallHelpers& jit, Typed::LanguageOperation operation)
{
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    constexpr GPRReg list = argumentGPR(0);
    constexpr GPRReg keptList = T11;
    constexpr GPRReg storage = T13;
    constexpr GPRReg mode = argumentGPR(1);
    constexpr GPRReg expected = argumentGPR(2);
    constexpr FPRReg number = FPRInfo::fpRegT0;
    static_assert(keptList != calleeGPR && keptList != thisGPR && keptList != countGPR && storage != calleeGPR && storage != thisGPR && storage != countGPR);
    CCallHelpers::JumpList generally;
    jit.move(list, keptList);
    generally.append(jit.branchIfNotCell(list));
    jit.load8(Address(list, JSCell::indexingTypeAndMiscOffset()), mode);
    jit.move(mode, expected);
    jit.urshift32(TrustedImm32(Instance::arrayKindShift), expected);
    jit.and32(TrustedImm32(Instance::numberOfArrayKinds - 1), expected);
    jit.load32(CCallHelpers::BaseIndex(instanceGPR, expected, CCallHelpers::TimesFour, Instance::offsetOfOriginalArrayStructureIDs()), expected);
    generally.append(jit.branch32(CCallHelpers::NotEqual, expected, Address(list, JSCell::structureIDOffset())));
    jit.loadPtr(Address(list, JSObject::butterflyOffset()), storage);
    jit.load32(Address(storage, Butterfly::offsetOfPublicLength()), countGPR);
    generally.append(jit.branch32(CCallHelpers::Above, countGPR, TrustedImm32(numberOfArgumentGPRs)));
    jit.and32(TrustedImm32(IndexingShapeMask), mode);
    Jump holdsDoubles = jit.branch32(CCallHelpers::Equal, mode, TrustedImm32(DoubleShape));
    Jump isContiguous = jit.branch32(CCallHelpers::Equal, mode, TrustedImm32(ContiguousShape));
    generally.append(jit.branch32(CCallHelpers::NotEqual, mode, TrustedImm32(Int32Shape)));
    isContiguous.link(&jit);
    std::array<CCallHelpers::JumpList, numberOfArgumentGPRs> isPastLength;
    for (unsigned i = 0; i < numberOfArgumentGPRs; ++i) {
        isPastLength[i].append(jit.branch32(CCallHelpers::BelowOrEqual, countGPR, TrustedImm32(i)));
        jit.load64(Address(storage, i * sizeof(EncodedJSValue)), argumentGPR(i));
        generally.append(jit.branchTest64(CCallHelpers::Zero, argumentGPR(i)));
    }
    jit.jump().linkTo(stubLabels()[static_cast<unsigned>(Stub::Call)], &jit);

    holdsDoubles.link(&jit);
    for (unsigned i = 0; i < numberOfArgumentGPRs; ++i) {
        isPastLength[i].append(jit.branch32(CCallHelpers::BelowOrEqual, countGPR, TrustedImm32(i)));
        jit.loadDouble(Address(storage, i * sizeof(double)), number);
        generally.append(jit.branchIfNaN(number));
        jit.moveDoubleTo64(number, argumentGPR(i));
        jit.sub64(numberTag, argumentGPR(i));
    }
    jit.jump().linkTo(stubLabels()[static_cast<unsigned>(Stub::Call)], &jit);

    for (unsigned i = 0; i < numberOfArgumentGPRs; ++i) {
        isPastLength[i].link(&jit);
        jit.move(CCallHelpers::TrustedImm64(JSValue::ValueUndefined), argumentGPR(i));
    }
    jit.jump().linkTo(stubLabels()[static_cast<unsigned>(Stub::Call)], &jit);

    generally.link(&jit);
    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(4 * sizeof(EncodedJSValue)), sp);
    jit.store64(calleeGPR, Address(sp, 0));
    jit.store64(thisGPR, Address(sp, sizeof(EncodedJSValue)));
    jit.store64(keptList, Address(sp, 2 * sizeof(EncodedJSValue)));
    jit.move(instanceGPR, A0);
    jit.move(TrustedImm32(static_cast<int32_t>(operation)), A1);
    jit.move(TrustedImm32(3), A2);
    jit.move(sp, A3);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), operationGPR);
    jit.loadPtr(Address(operationGPR, static_cast<unsigned>(Entry::operationAOTTypedLanguageOperation) * sizeof(void*)), operationGPR);
    prepareCallOperation(jit, assertionScratchGPR);
    jit.call(operationGPR, OperationPtrTag);
    jit.emitFunctionEpilogue();
    Jump exception = jit.branchTestPtr(CCallHelpers::NonZero, GPRInfo::returnValueGPR2);
    jit.ret();
    exception.link(&jit);
    loadInstance(jit, T9);
    jumpToEntry(jit, T9, Entry::HandleException);
}
static void generateApplyArray(CCallHelpers& jit) { generateCallWithArray(jit, Typed::LanguageOperation::Apply); }
static void generateSpreadArray(CCallHelpers& jit) { generateCallWithArray(jit, Typed::LanguageOperation::CallWithSpread); }
static void generateConstructClosure(CCallHelpers& jit) { jit.breakpoint(); }
static void generateCallHostFunction(CCallHelpers& jit) { jit.breakpoint(); }
static void generateCallInternalFunction(CCallHelpers& jit) { jit.breakpoint(); }
static void generateConstructInternalFunction(CCallHelpers& jit) { jit.breakpoint(); }

namespace IntrinsicRegisters {
constexpr GPRReg callee = calleeGPR;
constexpr GPRReg receiver = thisGPR;
constexpr GPRReg first = argumentGPR(0);
constexpr GPRReg second = argumentGPR(1);
constexpr GPRReg result = GPRInfo::returnValueGPR;
constexpr GPRReg scratch0 = A2;
constexpr GPRReg scratch1 = A3;
constexpr GPRReg scratch2 = T11;
constexpr GPRReg scratch3 = T9;
static_assert(noOverlap(callee, receiver, first, second, scratch0, scratch1, scratch2, scratch3));
}

struct MapOrSetLookup {
    static constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    static constexpr GPRReg key = IntrinsicRegisters::first;
#if CPU(X86_64)
    static constexpr GPRReg data = X86Registers::edx;
    static constexpr GPRReg count = X86Registers::ecx;
    static constexpr GPRReg slot = X86Registers::r9;
    static constexpr GPRReg entryKey = X86Registers::r10;
    static constexpr GPRReg keyImpl = X86Registers::ebx;
    static constexpr GPRReg entryImpl = X86Registers::r12;
    static constexpr GPRReg scratch = X86Registers::eax;
    static constexpr GPRReg scratch2 = X86Registers::esi;
    static constexpr GPRReg scratch3 = X86Registers::r8;
    static constexpr int32_t savedReceiver = 0;
    static constexpr int32_t savedSecond = 8;
    static constexpr int32_t savedCallee = 16;
    static constexpr int32_t savedKeyImplRegister = 24;
    static constexpr int32_t savedEntryImplRegister = 32;
    static constexpr int32_t savedHash = 40;
    static constexpr int32_t savedDeleted = 48;
    static constexpr int32_t frameSize = 56;
#else
    static constexpr GPRReg data = A4;
    static constexpr GPRReg count = A5;
    static constexpr GPRReg slot = GPRInfo::argumentGPR6;
    static constexpr GPRReg entryKey = GPRInfo::argumentGPR7;
    static constexpr GPRReg keyImpl = T14;
    static constexpr GPRReg entryImpl = T15;
    static constexpr GPRReg scratch = T12;
    static constexpr GPRReg scratch2 = A2;
    static constexpr GPRReg scratch3 = T9;
    static constexpr GPRReg deleted = T11;
    static constexpr GPRReg hash = T13;
#endif

    static void enter(CCallHelpers& jit)
    {
#if CPU(X86_64)
        jit.subPtr(TrustedImm32(frameSize), sp);
        jit.store64(IntrinsicRegisters::receiver, Address(sp, savedReceiver));
        jit.store64(IntrinsicRegisters::second, Address(sp, savedSecond));
        jit.store64(IntrinsicRegisters::callee, Address(sp, savedCallee));
        jit.store64(keyImpl, Address(sp, savedKeyImplRegister));
        jit.store64(entryImpl, Address(sp, savedEntryImplRegister));
#else
        UNUSED_PARAM(jit);
#endif
    }

    static void leave(CCallHelpers& jit)
    {
#if CPU(X86_64)
        jit.load64(Address(sp, savedKeyImplRegister), keyImpl);
        jit.load64(Address(sp, savedEntryImplRegister), entryImpl);
        jit.addPtr(TrustedImm32(frameSize), sp);
#else
        UNUSED_PARAM(jit);
#endif
    }

    static void leaveAsEntered(CCallHelpers& jit)
    {
#if CPU(X86_64)
        jit.load64(Address(sp, savedReceiver), IntrinsicRegisters::receiver);
        jit.load64(Address(sp, savedSecond), IntrinsicRegisters::second);
        jit.load64(Address(sp, savedCallee), IntrinsicRegisters::callee);
#endif
        leave(jit);
    }

    static void loadReceiver(CCallHelpers& jit, GPRReg result)
    {
#if CPU(X86_64)
        jit.load64(Address(sp, savedReceiver), result);
#else
        jit.move(IntrinsicRegisters::receiver, result);
#endif
    }

    static void loadSecond(CCallHelpers& jit, GPRReg result)
    {
#if CPU(X86_64)
        jit.load64(Address(sp, savedSecond), result);
#else
        jit.move(IntrinsicRegisters::second, result);
#endif
    }

    static void loadCallee(CCallHelpers& jit, GPRReg result)
    {
#if CPU(X86_64)
        jit.load64(Address(sp, savedCallee), result);
#else
        jit.move(IntrinsicRegisters::callee, result);
#endif
    }

    static void loadHash(CCallHelpers& jit, GPRReg result)
    {
#if CPU(X86_64)
        jit.load32(Address(sp, savedHash), result);
#else
        jit.move(hash, result);
#endif
    }
};

static void compareLatin1Characters(CCallHelpers& jit, GPRReg first, GPRReg second, GPRReg length, GPRReg scratch, GPRReg scratch2, CCallHelpers::JumpList& areSame, CCallHelpers::JumpList& differ)
{
    using BaseIndex = CCallHelpers::BaseIndex;
    Jump isBelow8 = jit.branch32(CCallHelpers::Below, length, TrustedImm32(8));
    CCallHelpers::Label loop = jit.label();
    jit.sub32(TrustedImm32(8), length);
    jit.load64(BaseIndex(first, length, CCallHelpers::TimesOne), scratch);
    jit.load64(BaseIndex(second, length, CCallHelpers::TimesOne), scratch2);
    differ.append(jit.branch64(CCallHelpers::NotEqual, scratch, scratch2));
    jit.branch32(CCallHelpers::AboveOrEqual, length, TrustedImm32(8)).linkTo(loop, &jit);
    jit.load64(Address(first), scratch);
    jit.load64(Address(second), scratch2);
    areSame.append(jit.branch64(CCallHelpers::Equal, scratch, scratch2));
    differ.append(jit.jump());

    isBelow8.link(&jit);
    Jump isBelow4 = jit.branch32(CCallHelpers::Below, length, TrustedImm32(4));
    jit.load32(Address(first), scratch);
    jit.load32(Address(second), scratch2);
    differ.append(jit.branch32(CCallHelpers::NotEqual, scratch, scratch2));
    jit.sub32(TrustedImm32(4), length);
    jit.load32(BaseIndex(first, length, CCallHelpers::TimesOne), scratch);
    jit.load32(BaseIndex(second, length, CCallHelpers::TimesOne), scratch2);
    areSame.append(jit.branch32(CCallHelpers::Equal, scratch, scratch2));
    differ.append(jit.jump());

    isBelow4.link(&jit);
    CCallHelpers::Label compare = jit.label();
    areSame.append(jit.branchTest32(CCallHelpers::Zero, length));
    jit.sub32(TrustedImm32(1), length);
    jit.load8(BaseIndex(first, length, CCallHelpers::TimesOne), scratch);
    jit.load8(BaseIndex(second, length, CCallHelpers::TimesOne), scratch2);
    jit.branch32(CCallHelpers::Equal, scratch, scratch2).linkTo(compare, &jit);
    differ.append(jit.jump());
}

// Where a Typed::CompactTable does not have the key, by the kind of its index. `data` is the table, `entryImpl` the address of the keys,
// `scratch2` what the index has of the hash.
struct MissesInCompactTable {
    // `slot` is the number of entries used.
    CCallHelpers::JumpList withTags;
    // `count` is the number of the free slot.
    CCallHelpers::JumpList withShortSlots;
    CCallHelpers::JumpList withLongSlots;
};

// The Typed::CompactTable is in `data`, its keys are narrow. The key is a string that is no rope and has been hashed: `keyImpl`, and the hash.
// Where the table has the key: the number of the entry is in `slot`, the address of the keys in `entryImpl`, and `data` is as it was.
static void findInCompactTable(CCallHelpers& jit, CCallHelpers::JumpList& otherwise, MissesInCompactTable& misses, CCallHelpers::JumpList& found, bool needsEntry, bool needsMisses, CCallHelpers::JumpList* sameCellByTags)
{
    using Lookup = MapOrSetLookup;
    using Table = Typed::CompactTable;
    using BaseIndex = CCallHelpers::BaseIndex;
    constexpr GPRReg key = Lookup::key;
    constexpr GPRReg keyImpl = Lookup::keyImpl;
    constexpr GPRReg table = Lookup::data;
    constexpr GPRReg keys = Lookup::entryImpl;
    constexpr GPRReg entry = Lookup::slot;
    constexpr GPRReg position = Lookup::count;
    constexpr GPRReg candidate = Lookup::entryKey;
    constexpr GPRReg tag = Lookup::scratch2;
    constexpr GPRReg mask = Lookup::scratch3;
    constexpr GPRReg scratch = Lookup::scratch;
    constexpr int32_t index = Table::offsetOfIndex();
    CCallHelpers::JumpList haveSameHash;

    auto compareWithEntry = [&](CCallHelpers::JumpList& next) {
        jit.load32(BaseIndex(keys, entry, CCallHelpers::TimesFour), candidate);
        next.append(jit.branchTest32(CCallHelpers::Zero, candidate));
        decodeCell32(jit, candidate, candidate);
        found.append(jit.branch64(CCallHelpers::Equal, candidate, key));
        jit.loadPtr(Address(candidate, JSString::offsetOfValue()), candidate);
        found.append(jit.branchPtr(CCallHelpers::Equal, candidate, keyImpl));
        jit.load32(Address(candidate, StringImpl::flagsOffset()), scratch);
        jit.urshift32(TrustedImm32(StringImpl::s_flagCount), scratch);
#if CPU(X86_64)
        haveSameHash.append(jit.branch32(CCallHelpers::Equal, scratch, Address(Lookup::sp, Lookup::savedHash)));
#else
        haveSameHash.append(jit.branch32(CCallHelpers::Equal, scratch, Lookup::hash));
#endif
    };
    auto probe = [&](CCallHelpers::Scale scale, unsigned shiftOfTag, unsigned entryBits) {
        jit.getEffectiveAddress(BaseIndex(table, mask, scale, index + (1 << scale)), keys);
        Lookup::loadHash(jit, tag);
        jit.and32(tag, mask, position);
        jit.urshift32(TrustedImm32(shiftOfTag), tag);
        CCallHelpers::Label loop = jit.label();
        CCallHelpers::JumpList next;
        if (scale == CCallHelpers::TimesTwo) {
            jit.load16(BaseIndex(table, position, scale, index), entry);
            misses.withShortSlots.append(jit.branch32(CCallHelpers::Equal, entry, TrustedImm32(UINT16_MAX)));
        } else {
            jit.load32(BaseIndex(table, position, scale, index), entry);
            misses.withLongSlots.append(jit.branch32(CCallHelpers::Equal, entry, TrustedImm32(-1)));
        }
        jit.urshift32(entry, TrustedImm32(entryBits), scratch);
        next.append(jit.branch32(CCallHelpers::NotEqual, scratch, tag));
        jit.and32(TrustedImm32((1 << entryBits) - 1), entry);
        compareWithEntry(next);
        next.link(&jit);
        jit.add32(TrustedImm32(1), position);
        jit.and32(mask, position);
        jit.jump().linkTo(loop, &jit);
    };

#if CPU(ARM64)
    {
        using Assembler = ARM64Assembler;
        constexpr GPRReg hash = Lookup::hash;
        constexpr GPRReg ones = Lookup::deleted;
        constexpr GPRReg matches = mask;
        constexpr int32_t firstKey = index + Table::maxCapacityWithTags;
        constexpr unsigned maxIndexBits = Table::maxIndexBitsWithShortSlots;
        static_assert(Table::maxCapacityWithTags == sizeof(uint64_t) && Table::entryBitsOfShortSlot == 8);
        CCallHelpers::JumpList generally;
        auto branchIfSameCell = [&] {
            jit.m_assembler.eor<64>(scratch, candidate, key, Assembler::LSR, Typed::cell32Shift);
            return jit.branchTest32(CCallHelpers::Zero, scratch);
        };
        auto compareWithOtherCell = [&](CCallHelpers::JumpList& isSame, CCallHelpers::JumpList& hasSameHash) {
            jit.move(key, scratch);
            jit.insertBitField64(candidate, TrustedImm32(Typed::cell32Shift), TrustedImm32(32), scratch);
            jit.loadPtr(Address(scratch, JSString::offsetOfValue()), candidate);
            isSame.append(jit.branchPtr(CCallHelpers::Equal, candidate, keyImpl));
            jit.load32(Address(candidate, StringImpl::flagsOffset()), scratch);
            jit.urshift32(TrustedImm32(StringImpl::s_flagCount), scratch);
            hasSameHash.append(jit.branch32(CCallHelpers::Equal, scratch, hash));
        };
        auto entryOfMatch = [&] {
            jit.urshift32(position, TrustedImm32(3), entry);
            jit.addPtr(TrustedImm32(firstKey), table, keys);
        };

        jit.urshift32(hash, TrustedImm32(Table::shiftOfShortTag), tag);
        generally.append(jit.branchTest32(CCallHelpers::Zero, tag));
        jit.load8(Address(table, Table::offsetOfIndexBits()), position);
        Jump hasSlots = jit.branchTest32(CCallHelpers::NonZero, position);

        jit.load64(Address(table, index), candidate);
        jit.move(CCallHelpers::TrustedImm64(0x0101010101010101), ones);
        jit.mul64(tag, ones, matches);
        jit.xor64(candidate, matches);
        jit.sub64(matches, ones, candidate);
        jit.m_assembler.bic<64>(candidate, candidate, matches);
        jit.m_assembler.and_<64>(matches, candidate, LogicalImmediate::create64(0x8080808080808080));
        Jump hasNoMatch = jit.branchTest64(CCallHelpers::Zero, matches);
        jit.countTrailingZeros64(matches, position);
        jit.m_assembler.add<64>(scratch, table, position, Assembler::LSR, 1);
        jit.load32(Address(scratch, firstKey - 3), candidate);
        Jump isSameCell = branchIfSameCell();
        constexpr GPRReg used = ones;
        CCallHelpers::JumpList hasNoMoreMatches;
        jit.load32(Address(table, Table::offsetOfUsed()), used);
        entryOfMatch();
        CCallHelpers::Label examine = jit.label();
        Jump isDeleted = jit.branchTest32(CCallHelpers::Zero, candidate);
        compareWithOtherCell(found, haveSameHash);
        isDeleted.link(&jit);
        jit.m_assembler.sub<64>(scratch, matches, UInt12(1));
        jit.and64(scratch, matches);
        hasNoMoreMatches.append(jit.branchTest64(CCallHelpers::Zero, matches));
        jit.countTrailingZeros64(matches, position);
        jit.urshift32(position, TrustedImm32(3), entry);
        hasNoMoreMatches.append(jit.branch32(CCallHelpers::AboveOrEqual, entry, used));
        jit.load32(BaseIndex(keys, entry, CCallHelpers::TimesFour), candidate);
        found.append(branchIfSameCell());
        jit.jump().linkTo(examine, &jit);
        if (sameCellByTags)
            sameCellByTags->append(isSameCell);
        else if (needsEntry) {
            isSameCell.link(&jit);
            entryOfMatch();
            found.append(jit.jump());
        } else
            found.append(isSameCell);
        if (needsMisses) {
            hasNoMatch.link(&jit);
            jit.load32(Address(table, Table::offsetOfUsed()), entry);
            jit.addPtr(TrustedImm32(firstKey), table, keys);
            misses.withTags.append(jit.jump());
            hasNoMoreMatches.link(&jit);
            jit.move(used, entry);
            misses.withTags.append(jit.jump());
        } else {
            misses.withTags.append(hasNoMatch);
            misses.withTags.append(hasNoMoreMatches);
        }

        hasSlots.link(&jit);
        generally.append(jit.branch32(CCallHelpers::Above, position, TrustedImm32(maxIndexBits)));
        jit.move(TrustedImm32(-1), mask);
        jit.lshift32(position, mask);
        jit.m_assembler.bic<32>(position, hash, mask);
        jit.m_assembler.sub<64>(keys, table, mask, Assembler::SXTW, 1);
        CCallHelpers::JumpList next;
        CCallHelpers::JumpList isSame;
        CCallHelpers::JumpList hasSameHash;
        CCallHelpers::JumpList isFree;
        auto loadSlot = [&] {
            jit.m_assembler.add<64>(scratch, table, position, Assembler::LSL, 1);
            jit.load16(Address(scratch, index), entry);
            jit.add32(TrustedImm32(1), entry, scratch);
            isFree.append(jit.branchTest32(CCallHelpers::NonZero, scratch, TrustedImm32(1 << 16)));
        };
        auto loadCandidate = [&] {
            jit.m_assembler.add<64>(scratch, keys, entry, Assembler::UXTB, 2);
            jit.load32(Address(scratch, index), candidate);
            isSame.append(branchIfSameCell());
        };
        auto branchIfOtherTag = [&] {
            jit.urshift32(entry, TrustedImm32(Table::entryBitsOfShortSlot), scratch);
            return jit.branch32(CCallHelpers::NotEqual, scratch, tag);
        };
        loadSlot();
        loadCandidate();
        next.append(branchIfOtherTag());
        next.append(jit.branchTest32(CCallHelpers::Zero, candidate));
        compareWithOtherCell(isSame, hasSameHash);
        next.link(&jit);
        CCallHelpers::Label probeAgain = jit.label();
        jit.add32(TrustedImm32(1), position);
        jit.m_assembler.bic<32>(position, position, mask);
        loadSlot();
        branchIfOtherTag().linkTo(probeAgain, &jit);
        loadCandidate();
        jit.branchTest32(CCallHelpers::Zero, candidate).linkTo(probeAgain, &jit);
        compareWithOtherCell(isSame, hasSameHash);
        jit.jump().linkTo(probeAgain, &jit);
        auto entryOfSlot = [&] {
            jit.and32(TrustedImm32((1 << Table::entryBitsOfShortSlot) - 1), entry);
            jit.addPtr(TrustedImm32(index), keys);
        };
        if (needsEntry) {
            isSame.link(&jit);
            entryOfSlot();
            found.append(jit.jump());
        } else
            found.append(isSame);
        hasSameHash.link(&jit);
        entryOfSlot();
        haveSameHash.append(jit.jump());
        if (needsMisses) {
            isFree.link(&jit);
            jit.addPtr(TrustedImm32(index), keys);
            misses.withShortSlots.append(jit.jump());
        } else
            misses.withShortSlots.append(isFree);

        generally.link(&jit);
    }
#else
    UNUSED_PARAM(needsEntry);
    UNUSED_PARAM(needsMisses);
    UNUSED_PARAM(sameCellByTags);
#endif

    jit.load8(Address(table, Table::offsetOfIndexBits()), position);
    Jump hasSlots = jit.branchTest32(CCallHelpers::NonZero, position);

    {
        constexpr GPRReg used = mask;
        jit.addPtr(TrustedImm32(index + Table::maxCapacityWithTags), table, keys);
        jit.load32(Address(table, Table::offsetOfUsed()), used);
        Lookup::loadHash(jit, tag);
        jit.urshift32(TrustedImm32(Table::shiftOfShortTag), tag);
        jit.move(TrustedImm32(0), entry);
        CCallHelpers::Label loop = jit.label();
        CCallHelpers::JumpList next;
        misses.withTags.append(jit.branch32(CCallHelpers::AboveOrEqual, entry, used));
        jit.load8(BaseIndex(table, entry, CCallHelpers::TimesOne, index), scratch);
        next.append(jit.branch32(CCallHelpers::NotEqual, scratch, tag));
        compareWithEntry(next);
        next.link(&jit);
        jit.add32(TrustedImm32(1), entry);
        jit.jump().linkTo(loop, &jit);
    }

    hasSlots.link(&jit);
    jit.move(TrustedImm32(1), mask);
    jit.lshift32(position, mask);
    jit.sub32(TrustedImm32(1), mask);
    Jump hasLongSlots = jit.branch32(CCallHelpers::Above, Address(table, Table::offsetOfCapacity()), TrustedImm32(Table::maxCapacityWithShortSlots));
    probe(CCallHelpers::TimesTwo, Table::shiftOfShortTag, Table::entryBitsOfShortSlot);
    hasLongSlots.link(&jit);
    probe(CCallHelpers::TimesFour, Table::shiftOfLongTag, Table::entryBitsOfLongSlot);

    // Hardly ever another string.
    haveSameHash.link(&jit);
    jit.load32(Address(candidate, StringImpl::lengthMemoryOffset()), position);
    otherwise.append(jit.branch32(CCallHelpers::NotEqual, position, Address(keyImpl, StringImpl::lengthMemoryOffset())));
    otherwise.append(jit.branchTest32(CCallHelpers::Zero, Address(candidate, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    otherwise.append(jit.branchTest32(CCallHelpers::Zero, Address(keyImpl, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.loadPtr(Address(candidate, StringImpl::dataOffset()), candidate);
    jit.loadPtr(Address(keyImpl, StringImpl::dataOffset()), scratch);
    compareLatin1Characters(jit, candidate, scratch, position, tag, mask, found, otherwise);
}

// Without the last: where a Typed::CompactTable does not have the key, it is absent like elsewhere.
template<typename MapOrSet>
static void findInMapOrSet(CCallHelpers& jit, CCallHelpers::JumpList& otherwise, CCallHelpers::JumpList& absentCases, CCallHelpers::JumpList& foundInCompactTable, MissesInCompactTable* missesInCompactTable, CCallHelpers::JumpList* hasNoTable, bool keyIsStringByType, bool needsEntry, CCallHelpers::JumpList* sameCellByTags)
{
    using Helper = typename MapOrSet::Helper;
    using Lookup = MapOrSetLookup;
    constexpr GPRReg key = Lookup::key;
    constexpr GPRReg data = Lookup::data;
    constexpr GPRReg count = Lookup::count;
    constexpr GPRReg slot = Lookup::slot;
    constexpr GPRReg entryKey = Lookup::entryKey;
    constexpr GPRReg keyImpl = Lookup::keyImpl;
    constexpr GPRReg entryImpl = Lookup::entryImpl;
    constexpr GPRReg scratch = Lookup::scratch;
    constexpr GPRReg scratch2 = Lookup::scratch2;
    constexpr GPRReg scratch3 = Lookup::scratch3;
#if CPU(X86_64)
    constexpr GPRReg hash = count;
    constexpr GPRReg deleted = slot;
#else
    constexpr GPRReg hash = Lookup::hash;
    constexpr GPRReg deleted = Lookup::deleted;
#endif

    jit.loadPtr(Address(IntrinsicRegisters::receiver, MapOrSet::offsetOfStorage()), data);

    CCallHelpers::JumpList hashed;
    CCallHelpers::JumpList plain;
    Jump notCell = jit.branchIfNotCell(key);
    if (!keyIsStringByType) {
        Jump isString = jit.branchIfType(key, StringType);
        otherwise.append(jit.branchIfType(key, HeapBigIntType));
        plain.append(jit.jump());
        isString.link(&jit);
    }
    jit.loadPtr(Address(key, JSString::offsetOfValue()), keyImpl);
    otherwise.append(jit.branchTestPtr(CCallHelpers::NonZero, keyImpl, TrustedImm32(JSString::isRopeInPointer)));
    jit.load32(Address(keyImpl, StringImpl::flagsOffset()), hash);
    jit.urshift32(TrustedImm32(StringImpl::s_flagCount), hash);
    otherwise.append(jit.branchTest32(CCallHelpers::Zero, hash));
    hashed.append(jit.branchTestPtr(CCallHelpers::NonZero, data));
#if CPU(X86_64)
    jit.store32(hash, Address(Lookup::sp, Lookup::savedHash));
    Lookup::loadReceiver(jit, data);
    jit.loadPtr(Address(data, MapOrSet::offsetOfCompactTable()), data);
#else
    jit.loadPtr(Address(IntrinsicRegisters::receiver, MapOrSet::offsetOfCompactTable()), data);
#endif
    // With the last: the collection has nothing yet, and the key is a string that is flat and hashed.
    (hasNoTable ? *hasNoTable : absentCases).append(jit.branchTestPtr(CCallHelpers::Zero, data));
    if (Options::useAOTWideReferences())
        otherwise.append(jit.jump());
    else if (missesInCompactTable)
        findInCompactTable(jit, otherwise, *missesInCompactTable, foundInCompactTable, needsEntry, true, sameCellByTags);
    else {
        MissesInCompactTable misses;
        findInCompactTable(jit, otherwise, misses, foundInCompactTable, needsEntry, false, sameCellByTags);
        absentCases.append(misses.withTags);
        absentCases.append(misses.withShortSlots);
        absentCases.append(misses.withLongSlots);
    }

    notCell.link(&jit);
    Jump notNumber = jit.branchIfNotNumber(key);
    otherwise.append(jit.branchIfNotInt32(key));
    notNumber.link(&jit);
    plain.link(&jit);
    jit.move(key, hash);
    jit.rapidHashMix64(hash, scratch, scratch2);
    hashed.link(&jit);
#if CPU(X86_64)
    jit.store32(hash, Address(Lookup::sp, Lookup::savedHash));
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfVM()), deleted);
    jit.loadPtr(Address(deleted, VM::offsetOfOrderedHashTableDeletedValue()), deleted);
    jit.store64(deleted, Address(Lookup::sp, Lookup::savedDeleted));
#else
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfVM()), deleted);
    jit.loadPtr(Address(deleted, VM::offsetOfOrderedHashTableDeletedValue()), deleted);
#endif

    // The key is no string, and all keys of a Typed::CompactTable are.
    absentCases.append(jit.branchTestPtr(CCallHelpers::Zero, data));
    jit.addPtr(TrustedImm32(JSCellButterfly::offsetOfData()), data);
    jit.load32(Address(data, Helper::capacityIndex() * sizeof(uint64_t)), scratch);
    jit.sub32(TrustedImm32(1), scratch);
    jit.and32(hash, scratch);
    jit.add32(TrustedImm32(Helper::hashTableStartIndex()), scratch);
    jit.load64(CCallHelpers::BaseIndex(data, scratch, CCallHelpers::TimesEight), count);

    CCallHelpers::Label loop = jit.label();
    CCallHelpers::JumpList found;
    CCallHelpers::JumpList next;
    absentCases.append(jit.branchTest64(CCallHelpers::Zero, count));
    jit.zeroExtend32ToWord(count, count);
    jit.getEffectiveAddress(CCallHelpers::BaseIndex(data, count, CCallHelpers::TimesEight), slot);
    jit.load64(Address(slot), entryKey);
#if CPU(X86_64)
    next.append(jit.branch64(CCallHelpers::Equal, entryKey, Address(Lookup::sp, Lookup::savedDeleted)));
#else
    next.append(jit.branch64(CCallHelpers::Equal, entryKey, deleted));
#endif
    found.append(jit.branch64(CCallHelpers::Equal, entryKey, key));
    next.append(jit.branchIfNotCell(entryKey));
    next.append(jit.branchIfNotCell(key));
    next.append(jit.branchIfNotType(entryKey, StringType));
    next.append(jit.branchIfNotType(key, StringType));
    jit.loadPtr(Address(entryKey, JSString::offsetOfValue()), entryImpl);
    otherwise.append(jit.branchTestPtr(CCallHelpers::NonZero, entryImpl, TrustedImm32(JSString::isRopeInPointer)));
    found.append(jit.branchPtr(CCallHelpers::Equal, entryImpl, keyImpl));
    jit.load32(Address(entryImpl, StringImpl::flagsOffset()), scratch);
    jit.urshift32(scratch, TrustedImm32(StringImpl::s_flagCount), scratch2);
    otherwise.append(jit.branchTest32(CCallHelpers::Zero, scratch2));
#if CPU(X86_64)
    next.append(jit.branch32(CCallHelpers::NotEqual, scratch2, Address(Lookup::sp, Lookup::savedHash)));
#else
    next.append(jit.branch32(CCallHelpers::NotEqual, scratch2, hash));
#endif
    jit.load32(Address(entryImpl, StringImpl::lengthMemoryOffset()), count);
    next.append(jit.branch32(CCallHelpers::NotEqual, count, Address(keyImpl, StringImpl::lengthMemoryOffset())));
    otherwise.append(jit.branchTest32(CCallHelpers::Zero, scratch, TrustedImm32(StringImpl::flagIs8Bit())));
    otherwise.append(jit.branchTest32(CCallHelpers::Zero, Address(keyImpl, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.loadPtr(Address(entryImpl, StringImpl::dataOffset()), entryImpl);
    jit.loadPtr(Address(keyImpl, StringImpl::dataOffset()), scratch);
    compareLatin1Characters(jit, entryImpl, scratch, count, scratch2, scratch3, found, next);

    next.link(&jit);
    jit.load64(Address(slot, Helper::ChainOffset * sizeof(EncodedJSValue)), count);
    jit.jump().linkTo(loop, &jit);

    found.link(&jit);
}

enum class MapOrSetOperation : uint8_t { MapGet, MapHas, MapSet, SetHas, SetAdd, MapDelete, SetDelete };

static void generateMapOrSetOperation(CCallHelpers& jit, MapOrSetOperation operation, CCallHelpers::JumpList* needsHostFunction, bool keyIsStringByType = false)
{
    using Lookup = MapOrSetLookup;
    constexpr GPRReg result = IntrinsicRegisters::result;
    CCallHelpers::JumpList giveUp;
    CCallHelpers::JumpList absentCases;
    CCallHelpers::JumpList foundInCompactTable;
    MissesInCompactTable missesInCompactTable;
    CCallHelpers::JumpList isOfAnotherRealm;
    bool addsEntries = operation == MapOrSetOperation::MapSet || operation == MapOrSetOperation::SetAdd;
    bool isOfSet = operation == MapOrSetOperation::SetHas || operation == MapOrSetOperation::SetAdd || operation == MapOrSetOperation::SetDelete;
    using Table = Typed::CompactTable;
    using BaseIndex = CCallHelpers::BaseIndex;
    auto leaveAndReturn = [&](JSValue value) {
        Lookup::leave(jit);
        jit.move(CCallHelpers::TrustedImm64(JSValue::encode(value)), result);
        jit.ret();
    };
    auto leaveAndReturnReceiver = [&] {
        Lookup::loadReceiver(jit, result);
        Lookup::leave(jit);
        jit.ret();
    };
    auto deferToOperation = [&](Entry function, bool hasValue, bool hasHash) {
        static_assert(noOverlap(Lookup::key, A1, A3, A4) && A2 != IntrinsicRegisters::receiver && A2 != IntrinsicRegisters::second);
        if (needsHostFunction) {
            Lookup::loadCallee(jit, Lookup::scratch);
            jit.loadPtr(Address(Lookup::scratch, JSCallee::offsetOfScopeChain()), Lookup::scratch);
            isOfAnotherRealm.append(jit.branchPtr(CCallHelpers::NotEqual, Lookup::scratch, Address(instanceGPR, Instance::offsetOfGlobalObject())));
        }
        jit.move(Lookup::key, A2);
        if (hasValue)
            Lookup::loadSecond(jit, A3);
        if (hasHash)
            Lookup::loadHash(jit, hasValue ? A4 : A3);
        Lookup::loadReceiver(jit, A1);
        Lookup::leave(jit);
        jit.move(TrustedImm32(static_cast<unsigned>(function) * sizeof(void*)), T9);
        jit.jump().linkTo(stubLabels()[static_cast<unsigned>(Stub::OperationValueWithInstance)], &jit);
    };
    // What follows is about a Typed::CompactTable: `data`, with the address of its keys in `entryImpl` and the number of an entry in `slot`.
    //
    // The second argument becomes the value of the entry, if the table can hold it. A narrow table is only made where every block is in
    // the cage (State::State), and what is in no block is not aligned like a cell of one.
    auto storeValue = [&] {
        constexpr GPRReg values = Lookup::entryKey;
        constexpr GPRReg value = Lookup::scratch;
        jit.load32(Address(Lookup::data, Table::offsetOfCapacity()), values);
        jit.getEffectiveAddress(BaseIndex(Lookup::entryImpl, values, CCallHelpers::TimesFour), values);
        Lookup::loadSecond(jit, value);
        Jump isWide = jit.branch8(CCallHelpers::NotEqual, Address(Lookup::data, Table::offsetOfValueWidth()), TrustedImm32(static_cast<int32_t>(Table::Width::Narrow)));
        Jump isCell = jit.branchIfCell(value);
        giveUp.append(jit.branch64(CCallHelpers::Above, value, TrustedImm32(Typed::undefinedCell32)));
        Jump isEncoded = jit.jump();
        isCell.link(&jit);
        static_assert((MarkedBlock::atomSize - 1) & PreciseAllocation::halfAlignment);
        static_assert((1u << Typed::cell32Shift) <= MarkedBlock::atomSize);
        giveUp.append(jit.branchTest64(CCallHelpers::NonZero, value, TrustedImm32(MarkedBlock::atomSize - 1)));
        jit.urshift64(TrustedImm32(Typed::cell32Shift), value);
        isEncoded.link(&jit);
        jit.store32(value, BaseIndex(values, Lookup::slot, CCallHelpers::TimesFour));
        Jump isStored = jit.jump();
        isWide.link(&jit);
        jit.store64(value, BaseIndex(values, Lookup::slot, CCallHelpers::TimesEight));
        isStored.link(&jit);
    };
    auto barrierAndReturnReceiver = [&] {
        jit.load8(Address(Lookup::data, JSCell::cellStateOffset()), Lookup::scratch2);
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfVM()), Lookup::scratch3);
        Jump needsBarrier = jit.branch32(CCallHelpers::BelowOrEqual, Lookup::scratch2, Address(Lookup::scratch3, VM::offsetOfHeapBarrierThreshold()));
        leaveAndReturnReceiver();
        needsBarrier.link(&jit);
        jit.move(Lookup::data, T9);
        Lookup::loadReceiver(jit, result);
        Lookup::leave(jit);
        jit.jump().linkTo(writeBarrierSlowCase(), &jit);
    };
    // The key gets an entry behind the others, if there is room. What is behind them is zero until it is written, so the collector, which goes
    // by the number of entries used, sees an entry whole, in part or not at all, and is made to look again by the barrier.
    auto append = [&](bool hasValue) {
        constexpr GPRReg table = Lookup::data;
        constexpr GPRReg entry = Lookup::slot;
        constexpr GPRReg position = Lookup::count;
        constexpr GPRReg tag = Lookup::scratch2;
        constexpr int32_t index = Table::offsetOfIndex();
        CCallHelpers::JumpList isIndexed;
        auto prepare = [&] {
            giveUp.append(jit.branch32(CCallHelpers::AboveOrEqual, entry, Address(table, Table::offsetOfCapacity())));
            if (hasValue)
                storeValue();
        };
        missesInCompactTable.withShortSlots.link(&jit);
        jit.load32(Address(table, Table::offsetOfUsed()), entry);
        prepare();
        jit.lshift32(TrustedImm32(Table::entryBitsOfShortSlot), tag);
        jit.or32(entry, tag);
        jit.store16(tag, BaseIndex(table, position, CCallHelpers::TimesTwo, index));
        isIndexed.append(jit.jump());
        missesInCompactTable.withLongSlots.link(&jit);
        jit.load32(Address(table, Table::offsetOfUsed()), entry);
        prepare();
        jit.lshift32(TrustedImm32(Table::entryBitsOfLongSlot), tag);
        jit.or32(entry, tag);
        jit.store32(tag, BaseIndex(table, position, CCallHelpers::TimesFour, index));
        isIndexed.append(jit.jump());
        missesInCompactTable.withTags.link(&jit);
        prepare();
        jit.store8(tag, BaseIndex(table, entry, CCallHelpers::TimesOne, index));
        isIndexed.link(&jit);
        jit.urshift64(Lookup::key, TrustedImm32(Typed::cell32Shift), Lookup::scratch);
        jit.store32(Lookup::scratch, BaseIndex(Lookup::entryImpl, entry, CCallHelpers::TimesFour));
#if CPU(ARM64)
        static_assert(Table::offsetOfAlive() == Table::offsetOfUsed() + 4 && !(Table::offsetOfUsed() % 8));
        jit.load64(Address(table, Table::offsetOfUsed()), Lookup::scratch);
        jit.move(CCallHelpers::TrustedImm64(0x100000001ull), tag);
        jit.add64(tag, Lookup::scratch);
        jit.store64(Lookup::scratch, Address(table, Table::offsetOfUsed()));
#else
        jit.add32(TrustedImm32(1), Address(table, Table::offsetOfUsed()));
        jit.add32(TrustedImm32(1), Address(table, Table::offsetOfAlive()));
#endif
        barrierAndReturnReceiver();
    };

    // The first entry of a collection, where the value is a cell of a block, `undefined`, `null` or a boolean: the table is made here, as CompactTable::tryAdd() makes it. All else is absent like elsewhere.
    CCallHelpers::JumpList hasNoTable;
    bool makesTable = addsEntries && keyIsStringByType && !Options::useAOTWideReferences();
    auto makeTable = [&](bool hasValue) {
        constexpr GPRReg table = Lookup::slot;
        constexpr GPRReg allocator = Lookup::scratch3;
        constexpr GPRReg scratch = Lookup::scratch2;
        constexpr GPRReg value = Lookup::entryKey;
        constexpr GPRReg structureID = Lookup::data;
        constexpr GPRReg receiver = Lookup::scratch;
        static_assert(noOverlap(table, allocator, scratch, value, structureID, receiver, Lookup::key, Lookup::count));
        Table::Width valueWidth = hasValue ? Table::Width::Narrow : Table::Width::None;
        constexpr int32_t keys = Table::offsetOfIndex() + Table::maxCapacityWithTags;
        static_assert(Table::initialCapacity == 4 && Table::maxCapacityWithTags == sizeof(uint64_t));
        if (!makesTable)
            return;
        hasNoTable.link(&jit);
        if (hasValue) {
            Lookup::loadSecond(jit, value);
            Jump isCell = jit.branchIfCell(value);
            absentCases.append(jit.branch64(CCallHelpers::Above, value, TrustedImm32(Typed::undefinedCell32)));
            Jump isEncoded = jit.jump();
            isCell.link(&jit);
            static_assert((MarkedBlock::atomSize - 1) & PreciseAllocation::halfAlignment);
            static_assert((1u << Typed::cell32Shift) <= MarkedBlock::atomSize);
            absentCases.append(jit.branchTest64(CCallHelpers::NonZero, value, TrustedImm32(MarkedBlock::atomSize - 1)));
            jit.urshift64(TrustedImm32(Typed::cell32Shift), value);
            jit.zeroExtend32ToWord(value, value);
            isEncoded.link(&jit);
        }
        // What is stored has to be seen before the table is.
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfVM()), scratch);
        absentCases.append(jit.branchTest8(CCallHelpers::NonZero, Address(scratch, VM::offsetOfHeapMutatorShouldBeFenced())));
        jit.load32(Address(instanceGPR, Instance::offsetOfCompactTableStructureID()), structureID);
        absentCases.append(jit.branchTest32(CCallHelpers::Zero, structureID));
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfCellSpace()), allocator);
        jit.loadPtr(Address(allocator, CompleteSubspace::offsetOfAllocatorForSizeStep() + MarkedSpace::sizeClassToIndex(Table::sizeAtBirth(valueWidth)) * sizeof(Allocator)), allocator);
        absentCases.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
        jit.emitAllocateWithNonNullAllocator(table, JITAllocator::variable(), allocator, scratch, absentCases, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
        jit.store32(structureID, Address(table, JSCell::structureIDOffset()));
        jit.store32(TrustedImm32(TypeInfoBlob(NonArray, TypeInfo(CellType, Table::StructureFlags)).blob()), Address(table, JSCell::indexingTypeAndMiscOffset()));
        jit.store64(CCallHelpers::TrustedImm64(0x100000001ull), Address(table, Table::offsetOfUsed()));
        jit.store64(CCallHelpers::TrustedImm64(Table::capacityAndKindsAtBirth(valueWidth)), Address(table, Table::offsetOfCapacity()));
        Lookup::loadHash(jit, scratch);
        jit.urshift32(TrustedImm32(Table::shiftOfShortTag), scratch);
        jit.store64(scratch, Address(table, Table::offsetOfIndex()));
        jit.urshift64(Lookup::key, TrustedImm32(Typed::cell32Shift), scratch);
        jit.zeroExtend32ToWord(scratch, scratch);
        jit.store64(scratch, Address(table, keys));
        jit.store64(CCallHelpers::TrustedImm64(0), Address(table, keys + 8));
        if (hasValue) {
            jit.store64(value, Address(table, keys + 16));
            jit.store64(CCallHelpers::TrustedImm64(0), Address(table, keys + 24));
        }
        Lookup::loadReceiver(jit, receiver);
        jit.storePtr(table, Address(receiver, isOfSet ? JSSet::offsetOfCompactTable() : JSMap::offsetOfCompactTable()));
        jit.load8(Address(receiver, JSCell::cellStateOffset()), scratch);
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfVM()), allocator);
        Jump needsBarrier = jit.branch32(CCallHelpers::BelowOrEqual, scratch, Address(allocator, VM::offsetOfHeapBarrierThreshold()));
        leaveAndReturnReceiver();
        needsBarrier.link(&jit);
        jit.move(receiver, T9);
        Lookup::loadReceiver(jit, result);
        Lookup::leave(jit);
        jit.jump().linkTo(writeBarrierSlowCase(), &jit);
    };

    Lookup::enter(jit);
    bool needsEntry = operation != MapOrSetOperation::MapHas && operation != MapOrSetOperation::SetHas && operation != MapOrSetOperation::SetAdd;
    CCallHelpers::JumpList sameCellByTags;
    CCallHelpers::JumpList* sameCellByTagsIfWanted = isARM64() && operation == MapOrSetOperation::MapGet ? &sameCellByTags : nullptr;
    if (isOfSet)
        findInMapOrSet<JSSet>(jit, giveUp, absentCases, foundInCompactTable, addsEntries ? &missesInCompactTable : nullptr, makesTable ? &hasNoTable : nullptr, keyIsStringByType, needsEntry, sameCellByTagsIfWanted);
    else
        findInMapOrSet<JSMap>(jit, giveUp, absentCases, foundInCompactTable, addsEntries ? &missesInCompactTable : nullptr, makesTable ? &hasNoTable : nullptr, keyIsStringByType, needsEntry, sameCellByTagsIfWanted);
    switch (operation) {
    case MapOrSetOperation::MapGet: {
        jit.load64(Address(Lookup::slot, sizeof(EncodedJSValue)), result);
        Lookup::leave(jit);
        jit.ret();
        constexpr GPRReg values = Lookup::entryImpl;
        foundInCompactTable.link(&jit);
        jit.load32(Address(Lookup::data, Table::offsetOfCapacity()), Lookup::scratch);
        jit.getEffectiveAddress(CCallHelpers::BaseIndex(values, Lookup::scratch, CCallHelpers::TimesFour), values);
        static_assert(static_cast<int32_t>(Table::Width::Wide) == 8 && !(static_cast<int32_t>(Table::Width::Narrow) & 8));
        Jump isWide = jit.branchTest8(CCallHelpers::NonZero, Address(Lookup::data, Table::offsetOfValueWidth()), TrustedImm32(static_cast<int32_t>(Table::Width::Wide)));
#if CPU(ARM64)
        static_assert(result == Lookup::key);
        constexpr GPRReg narrow = Lookup::scratch;
        auto widenAndReturn = [&] {
            Jump isNoCell = jit.branch32(CCallHelpers::BelowOrEqual, narrow, TrustedImm32(Typed::undefinedCell32));
            jit.insertBitField64(narrow, TrustedImm32(Typed::cell32Shift), TrustedImm32(32), result);
            jit.ret();
            isNoCell.link(&jit);
            jit.move(narrow, result);
            jit.ret();
        };
        jit.load32(CCallHelpers::BaseIndex(values, Lookup::slot, CCallHelpers::TimesFour), narrow);
        widenAndReturn();
        constexpr int32_t firstKey = Table::offsetOfIndex() + Table::maxCapacityWithTags;
        sameCellByTags.link(&jit);
        jit.load32(Address(Lookup::data, Table::offsetOfCapacity()), Lookup::scratch);
        jit.m_assembler.add<64>(values, Lookup::data, Lookup::scratch, ARM64Assembler::LSL, 2);
        Jump isWideByTags = jit.branchTest8(CCallHelpers::NonZero, Address(Lookup::data, Table::offsetOfValueWidth()), TrustedImm32(static_cast<int32_t>(Table::Width::Wide)));
        jit.m_assembler.add<64>(values, values, Lookup::count, ARM64Assembler::LSR, 1);
        jit.load32(Address(values, firstKey - 3), narrow);
        widenAndReturn();
        isWideByTags.link(&jit);
        jit.add64(Lookup::count, values);
        jit.load64(Address(values, firstKey - 7), result);
        jit.ret();
#else
        jit.load32(CCallHelpers::BaseIndex(values, Lookup::slot, CCallHelpers::TimesFour), result);
        Jump isNoCell = jit.branch32(CCallHelpers::BelowOrEqual, result, TrustedImm32(Typed::undefinedCell32));
        decodeCell32(jit, result, result);
        isNoCell.link(&jit);
        Lookup::leave(jit);
        jit.ret();
#endif
        isWide.link(&jit);
        jit.load64(CCallHelpers::BaseIndex(values, Lookup::slot, CCallHelpers::TimesEight), result);
        Lookup::leave(jit);
        jit.ret();
        absentCases.link(&jit);
        leaveAndReturn(jsUndefined());
        giveUp.link(&jit);
        deferToOperation(Entry::operationAOTMapGet, false, false);
        break;
    }
    case MapOrSetOperation::MapHas:
    case MapOrSetOperation::SetHas:
        foundInCompactTable.link(&jit);
        leaveAndReturn(jsBoolean(true));
        absentCases.link(&jit);
        leaveAndReturn(jsBoolean(false));
        giveUp.link(&jit);
        deferToOperation(operation == MapOrSetOperation::MapHas ? Entry::operationAOTMapHas : Entry::operationAOTSetHas, false, false);
        break;
    case MapOrSetOperation::MapSet: {
        Lookup::loadSecond(jit, Lookup::scratch2);
        jit.store64(Lookup::scratch2, Address(Lookup::slot, sizeof(EncodedJSValue)));
        Jump notCell = jit.branchIfNotCell(Lookup::scratch2);
        Lookup::loadReceiver(jit, Lookup::scratch);
        jit.loadPtr(Address(Lookup::scratch, JSMap::offsetOfStorage()), Lookup::scratch);
        jit.load8(Address(Lookup::scratch, JSCell::cellStateOffset()), Lookup::scratch2);
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfVM()), Lookup::scratch3);
        Jump needsBarrier = jit.branch32(CCallHelpers::BelowOrEqual, Lookup::scratch2, Address(Lookup::scratch3, VM::offsetOfHeapBarrierThreshold()));
        notCell.link(&jit);
        leaveAndReturnReceiver();
        needsBarrier.link(&jit);
        jit.move(Lookup::scratch, T9);
        Lookup::loadReceiver(jit, result);
        Lookup::leave(jit);
        jit.jump().linkTo(writeBarrierSlowCase(), &jit);
        foundInCompactTable.link(&jit);
        storeValue();
        barrierAndReturnReceiver();
        append(true);
        makeTable(true);
        absentCases.link(&jit);
        if (!keyIsStringByType)
            deferToOperation(Entry::operationAOTMapSet, true, true);
        giveUp.link(&jit);
        deferToOperation(keyIsStringByType ? Entry::operationAOTMapSetWithStringKey : Entry::operationAOTMapSetGeneric, true, false);
        break;
    }
    // An entry of the engine's storage is left to the engine. In a Typed::CompactTable key and value are cleared, and what led to the entry stays.
    case MapOrSetOperation::MapDelete:
    case MapOrSetOperation::SetDelete: {
        Entry function = isOfSet ? Entry::operationAOTTypedSetDelete : Entry::operationAOTTypedMapDelete;
        deferToOperation(function, false, false);
        foundInCompactTable.link(&jit);
        jit.store32(TrustedImm32(0), BaseIndex(Lookup::entryImpl, Lookup::slot, CCallHelpers::TimesFour));
        if (!isOfSet) {
            constexpr GPRReg values = Lookup::entryKey;
            jit.load32(Address(Lookup::data, Table::offsetOfCapacity()), values);
            jit.getEffectiveAddress(BaseIndex(Lookup::entryImpl, values, CCallHelpers::TimesFour), values);
            Jump isWide = jit.branch8(CCallHelpers::NotEqual, Address(Lookup::data, Table::offsetOfValueWidth()), TrustedImm32(static_cast<int32_t>(Table::Width::Narrow)));
            jit.store32(TrustedImm32(0), BaseIndex(values, Lookup::slot, CCallHelpers::TimesFour));
            Jump isCleared = jit.jump();
            isWide.link(&jit);
            jit.store64(CCallHelpers::TrustedImm64(0), BaseIndex(values, Lookup::slot, CCallHelpers::TimesEight));
            isCleared.link(&jit);
        }
        jit.sub32(TrustedImm32(1), Address(Lookup::data, Table::offsetOfAlive()));
        leaveAndReturn(jsBoolean(true));
        absentCases.link(&jit);
        leaveAndReturn(jsBoolean(false));
        giveUp.link(&jit);
        deferToOperation(function, false, false);
        break;
    }
    case MapOrSetOperation::SetAdd:
        foundInCompactTable.link(&jit);
        leaveAndReturnReceiver();
        append(false);
        makeTable(false);
        absentCases.link(&jit);
        if (!keyIsStringByType)
            deferToOperation(Entry::operationAOTSetAdd, false, true);
        giveUp.link(&jit);
        deferToOperation(keyIsStringByType ? Entry::operationAOTSetAddWithStringKey : Entry::operationAOTSetAddGeneric, false, false);
        break;
    }
    if (needsHostFunction) {
        isOfAnotherRealm.link(&jit);
        Lookup::leaveAsEntered(jit);
        needsHostFunction->append(jit.jump());
    }
}

static void generateLookupInCompactTableWithoutFrame(CCallHelpers& jit, bool isOfSet, bool yieldsValue, bool keyIsStringByType = false)
{
#if CPU(X86_64)
    if (Options::useAOTWideReferences())
        return;
    using Table = Typed::CompactTable;
    using BaseIndex = CCallHelpers::BaseIndex;
    constexpr GPRReg receiver = IntrinsicRegisters::receiver;
    constexpr GPRReg key = IntrinsicRegisters::first;
    constexpr GPRReg result = IntrinsicRegisters::result;
    constexpr GPRReg table = X86Registers::edx;
    constexpr GPRReg position = X86Registers::ecx;
    constexpr GPRReg tag = X86Registers::esi;
    constexpr GPRReg mask = X86Registers::r8;
    constexpr GPRReg entry = X86Registers::r9;
    constexpr GPRReg candidate = X86Registers::r10;
    constexpr GPRReg keys = X86Registers::r11;
    static_assert(noOverlap(receiver, key, table, position, tag, mask, entry, candidate, keys, instanceGPR));
    constexpr int32_t index = Table::offsetOfIndex();
    CCallHelpers::JumpList isLeftToTheRest;
    CCallHelpers::JumpList found;
    CCallHelpers::JumpList absent;

    isLeftToTheRest.append(jit.branchIfNotCell(key));
    if (!keyIsStringByType)
        isLeftToTheRest.append(jit.branchIfNotType(key, StringType));
    jit.loadPtr(Address(key, JSString::offsetOfValue()), candidate);
    isLeftToTheRest.append(jit.branchTestPtr(CCallHelpers::NonZero, candidate, TrustedImm32(JSString::isRopeInPointer)));
    jit.load32(Address(candidate, StringImpl::flagsOffset()), tag);
    jit.urshift32(TrustedImm32(StringImpl::s_flagCount), tag);
    isLeftToTheRest.append(jit.branchTest32(CCallHelpers::Zero, tag));
    jit.loadPtr(Address(receiver, isOfSet ? JSSet::offsetOfCompactTable() : JSMap::offsetOfCompactTable()), table);
    isLeftToTheRest.append(jit.branchTestPtr(CCallHelpers::Zero, table));

    auto compareWithEntry = [&](CCallHelpers::JumpList& next) {
        jit.load32(BaseIndex(keys, entry, CCallHelpers::TimesFour), candidate);
        next.append(jit.branchTest32(CCallHelpers::Zero, candidate));
        decodeCell32(jit, candidate, candidate);
        found.append(jit.branch64(CCallHelpers::Equal, candidate, key));
        jit.loadPtr(Address(candidate, JSString::offsetOfValue()), candidate);
        found.append(jit.branch64(CCallHelpers::Equal, candidate, Address(key, JSString::offsetOfValue())));
        isLeftToTheRest.append(jit.jump());
    };
    auto probe = [&](CCallHelpers::Scale scale, unsigned shiftOfTag, unsigned entryBits) {
        jit.getEffectiveAddress(BaseIndex(table, mask, scale, index + (1 << scale)), keys);
        jit.and32(tag, mask, position);
        jit.urshift32(TrustedImm32(shiftOfTag), tag);
        CCallHelpers::Label loop = jit.label();
        CCallHelpers::JumpList next;
        if (scale == CCallHelpers::TimesTwo) {
            jit.load16(BaseIndex(table, position, scale, index), entry);
            absent.append(jit.branch32(CCallHelpers::Equal, entry, TrustedImm32(UINT16_MAX)));
        } else {
            jit.load32(BaseIndex(table, position, scale, index), entry);
            absent.append(jit.branch32(CCallHelpers::Equal, entry, TrustedImm32(-1)));
        }
        jit.urshift32(entry, TrustedImm32(entryBits), candidate);
        next.append(jit.branch32(CCallHelpers::NotEqual, candidate, tag));
        jit.and32(TrustedImm32((1 << entryBits) - 1), entry);
        compareWithEntry(next);
        next.link(&jit);
        jit.add32(TrustedImm32(1), position);
        jit.and32(mask, position);
        jit.jump().linkTo(loop, &jit);
    };

    jit.load8(Address(table, Table::offsetOfIndexBits()), position);
    Jump hasSlots = jit.branchTest32(CCallHelpers::NonZero, position);
    {
        constexpr GPRReg used = mask;
        jit.addPtr(TrustedImm32(index + Table::maxCapacityWithTags), table, keys);
        jit.load32(Address(table, Table::offsetOfUsed()), used);
        jit.urshift32(TrustedImm32(Table::shiftOfShortTag), tag);
        jit.move(TrustedImm32(0), entry);
        CCallHelpers::Label loop = jit.label();
        CCallHelpers::JumpList next;
        absent.append(jit.branch32(CCallHelpers::AboveOrEqual, entry, used));
        jit.load8(BaseIndex(table, entry, CCallHelpers::TimesOne, index), candidate);
        next.append(jit.branch32(CCallHelpers::NotEqual, candidate, tag));
        compareWithEntry(next);
        next.link(&jit);
        jit.add32(TrustedImm32(1), entry);
        jit.jump().linkTo(loop, &jit);
    }
    hasSlots.link(&jit);
    jit.move(TrustedImm32(1), mask);
    jit.lshift32(position, mask);
    jit.sub32(TrustedImm32(1), mask);
    Jump hasLongSlots = jit.branch32(CCallHelpers::Above, Address(table, Table::offsetOfCapacity()), TrustedImm32(Table::maxCapacityWithShortSlots));
    probe(CCallHelpers::TimesTwo, Table::shiftOfShortTag, Table::entryBitsOfShortSlot);
    hasLongSlots.link(&jit);
    probe(CCallHelpers::TimesFour, Table::shiftOfLongTag, Table::entryBitsOfLongSlot);

    found.link(&jit);
    if (yieldsValue) {
        jit.load32(Address(table, Table::offsetOfCapacity()), position);
        jit.getEffectiveAddress(BaseIndex(keys, position, CCallHelpers::TimesFour), keys);
        Jump isWide = jit.branch8(CCallHelpers::NotEqual, Address(table, Table::offsetOfValueWidth()), TrustedImm32(static_cast<int32_t>(Table::Width::Narrow)));
        jit.load32(BaseIndex(keys, entry, CCallHelpers::TimesFour), result);
        Jump isNoCell = jit.branch32(CCallHelpers::BelowOrEqual, result, TrustedImm32(Typed::undefinedCell32));
        decodeCell32(jit, result, result);
        isNoCell.link(&jit);
        jit.ret();
        isWide.link(&jit);
        jit.load64(BaseIndex(keys, entry, CCallHelpers::TimesEight), result);
        jit.ret();
    } else {
        jit.move(CCallHelpers::TrustedImm64(JSValue::encode(jsBoolean(true))), result);
        jit.ret();
    }
    absent.link(&jit);
    jit.move(CCallHelpers::TrustedImm64(JSValue::encode(yieldsValue ? jsUndefined() : jsBoolean(false))), result);
    jit.ret();
    isLeftToTheRest.link(&jit);
#else
    UNUSED_PARAM(jit);
    UNUSED_PARAM(isOfSet);
    UNUSED_PARAM(yieldsValue);
    UNUSED_PARAM(keyIsStringByType);
#endif
}

static void generateStoreInCompactTableWithoutFrame(CCallHelpers& jit, bool isOfSet)
{
#if CPU(X86_64)
    if (Options::useAOTWideReferences())
        return;
    using Table = Typed::CompactTable;
    using BaseIndex = CCallHelpers::BaseIndex;
    constexpr GPRReg receiver = IntrinsicRegisters::receiver;
    constexpr GPRReg key = IntrinsicRegisters::first;
    constexpr GPRReg stored = IntrinsicRegisters::second;
    constexpr GPRReg table = X86Registers::edx;
    constexpr GPRReg position = X86Registers::ecx;
    constexpr GPRReg tag = X86Registers::r12;
    constexpr GPRReg mask = X86Registers::r8;
    constexpr GPRReg entry = X86Registers::r9;
    constexpr GPRReg candidate = X86Registers::r10;
    constexpr GPRReg keys = X86Registers::r11;
    static_assert(receiver == IntrinsicRegisters::result);
    static_assert(noOverlap(receiver, key, stored, table, position, tag, mask, entry, candidate, keys, instanceGPR));
    constexpr int32_t index = Table::offsetOfIndex();
    CCallHelpers::JumpList isLeftToTheRest;
    CCallHelpers::JumpList found;
    CCallHelpers::JumpList isAbsentBehindTags;
    CCallHelpers::JumpList hasFreeShortSlot;
    CCallHelpers::JumpList hasFreeLongSlot;
    CCallHelpers::JumpList isIndexed;

    isLeftToTheRest.append(jit.branchIfNotCell(key));
    jit.loadPtr(Address(key, JSString::offsetOfValue()), candidate);
    isLeftToTheRest.append(jit.branchTestPtr(CCallHelpers::NonZero, candidate, TrustedImm32(JSString::isRopeInPointer)));
    jit.load32(Address(candidate, StringImpl::flagsOffset()), tag);
    jit.urshift32(TrustedImm32(StringImpl::s_flagCount), tag);
    isLeftToTheRest.append(jit.branchTest32(CCallHelpers::Zero, tag));
    jit.loadPtr(Address(receiver, isOfSet ? JSSet::offsetOfCompactTable() : JSMap::offsetOfCompactTable()), table);
    isLeftToTheRest.append(jit.branchTestPtr(CCallHelpers::Zero, table));

    auto compareWithEntry = [&](CCallHelpers::JumpList& next) {
        jit.load32(BaseIndex(keys, entry, CCallHelpers::TimesFour), candidate);
        next.append(jit.branchTest32(CCallHelpers::Zero, candidate));
        decodeCell32(jit, candidate, candidate);
        found.append(jit.branch64(CCallHelpers::Equal, candidate, key));
        jit.loadPtr(Address(candidate, JSString::offsetOfValue()), candidate);
        found.append(jit.branch64(CCallHelpers::Equal, candidate, Address(key, JSString::offsetOfValue())));
        isLeftToTheRest.append(jit.jump());
    };
    auto probe = [&](CCallHelpers::Scale scale, unsigned shiftOfTag, unsigned entryBits) {
        jit.getEffectiveAddress(BaseIndex(table, mask, scale, index + (1 << scale)), keys);
        jit.and32(tag, mask, position);
        jit.urshift32(TrustedImm32(shiftOfTag), tag);
        CCallHelpers::Label loop = jit.label();
        CCallHelpers::JumpList next;
        if (scale == CCallHelpers::TimesTwo) {
            jit.load16(BaseIndex(table, position, scale, index), entry);
            hasFreeShortSlot.append(jit.branch32(CCallHelpers::Equal, entry, TrustedImm32(UINT16_MAX)));
        } else {
            jit.load32(BaseIndex(table, position, scale, index), entry);
            hasFreeLongSlot.append(jit.branch32(CCallHelpers::Equal, entry, TrustedImm32(-1)));
        }
        jit.urshift32(entry, TrustedImm32(entryBits), candidate);
        next.append(jit.branch32(CCallHelpers::NotEqual, candidate, tag));
        jit.and32(TrustedImm32((1 << entryBits) - 1), entry);
        compareWithEntry(next);
        next.link(&jit);
        jit.add32(TrustedImm32(1), position);
        jit.and32(mask, position);
        jit.jump().linkTo(loop, &jit);
    };
    auto storeValue = [&] {
        if (isOfSet)
            return;
        constexpr GPRReg values = mask;
        constexpr GPRReg value = candidate;
        jit.load32(Address(table, Table::offsetOfCapacity()), values);
        jit.getEffectiveAddress(BaseIndex(keys, values, CCallHelpers::TimesFour), values);
        jit.move(stored, value);
        Jump isWide = jit.branch8(CCallHelpers::NotEqual, Address(table, Table::offsetOfValueWidth()), TrustedImm32(static_cast<int32_t>(Table::Width::Narrow)));
        Jump isCell = jit.branchIfCell(value);
        isLeftToTheRest.append(jit.branch64(CCallHelpers::Above, value, TrustedImm32(Typed::undefinedCell32)));
        Jump isEncoded = jit.jump();
        isCell.link(&jit);
        isLeftToTheRest.append(jit.branchTest64(CCallHelpers::NonZero, value, TrustedImm32(MarkedBlock::atomSize - 1)));
        jit.urshift64(TrustedImm32(Typed::cell32Shift), value);
        isEncoded.link(&jit);
        jit.store32(value, BaseIndex(values, entry, CCallHelpers::TimesFour));
        Jump isStored = jit.jump();
        isWide.link(&jit);
        jit.store64(value, BaseIndex(values, entry, CCallHelpers::TimesEight));
        isStored.link(&jit);
    };
    auto barrierAndReturnReceiver = [&] {
        jit.load8(Address(table, JSCell::cellStateOffset()), candidate);
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfVM()), mask);
        Jump needsBarrier = jit.branch32(CCallHelpers::BelowOrEqual, candidate, Address(mask, VM::offsetOfHeapBarrierThreshold()));
        jit.ret();
        needsBarrier.link(&jit);
        jit.move(table, T9);
        jit.jump().linkTo(writeBarrierSlowCase(), &jit);
    };
    auto prepare = [&] {
        isLeftToTheRest.append(jit.branch32(CCallHelpers::AboveOrEqual, entry, Address(table, Table::offsetOfCapacity())));
        storeValue();
    };

    jit.load8(Address(table, Table::offsetOfIndexBits()), position);
    Jump hasSlots = jit.branchTest32(CCallHelpers::NonZero, position);
    {
        constexpr GPRReg used = mask;
        jit.addPtr(TrustedImm32(index + Table::maxCapacityWithTags), table, keys);
        jit.load32(Address(table, Table::offsetOfUsed()), used);
        jit.urshift32(TrustedImm32(Table::shiftOfShortTag), tag);
        jit.move(TrustedImm32(0), entry);
        CCallHelpers::Label loop = jit.label();
        CCallHelpers::JumpList next;
        isAbsentBehindTags.append(jit.branch32(CCallHelpers::AboveOrEqual, entry, used));
        jit.load8(BaseIndex(table, entry, CCallHelpers::TimesOne, index), candidate);
        next.append(jit.branch32(CCallHelpers::NotEqual, candidate, tag));
        compareWithEntry(next);
        next.link(&jit);
        jit.add32(TrustedImm32(1), entry);
        jit.jump().linkTo(loop, &jit);
    }
    hasSlots.link(&jit);
    jit.move(TrustedImm32(1), mask);
    jit.lshift32(position, mask);
    jit.sub32(TrustedImm32(1), mask);
    Jump hasLongSlots = jit.branch32(CCallHelpers::Above, Address(table, Table::offsetOfCapacity()), TrustedImm32(Table::maxCapacityWithShortSlots));
    probe(CCallHelpers::TimesTwo, Table::shiftOfShortTag, Table::entryBitsOfShortSlot);
    hasLongSlots.link(&jit);
    probe(CCallHelpers::TimesFour, Table::shiftOfLongTag, Table::entryBitsOfLongSlot);

    found.link(&jit);
    if (isOfSet)
        jit.ret();
    else {
        storeValue();
        barrierAndReturnReceiver();
    }

    hasFreeShortSlot.link(&jit);
    jit.load32(Address(table, Table::offsetOfUsed()), entry);
    prepare();
    jit.lshift32(TrustedImm32(Table::entryBitsOfShortSlot), tag);
    jit.or32(entry, tag);
    jit.store16(tag, BaseIndex(table, position, CCallHelpers::TimesTwo, index));
    isIndexed.append(jit.jump());
    hasFreeLongSlot.link(&jit);
    jit.load32(Address(table, Table::offsetOfUsed()), entry);
    prepare();
    jit.lshift32(TrustedImm32(Table::entryBitsOfLongSlot), tag);
    jit.or32(entry, tag);
    jit.store32(tag, BaseIndex(table, position, CCallHelpers::TimesFour, index));
    isIndexed.append(jit.jump());
    isAbsentBehindTags.link(&jit);
    prepare();
    jit.store8(tag, BaseIndex(table, entry, CCallHelpers::TimesOne, index));
    isIndexed.link(&jit);
    jit.urshift64(key, TrustedImm32(Typed::cell32Shift), candidate);
    jit.store32(candidate, BaseIndex(keys, entry, CCallHelpers::TimesFour));
    jit.add32(TrustedImm32(1), Address(table, Table::offsetOfUsed()));
    jit.add32(TrustedImm32(1), Address(table, Table::offsetOfAlive()));
    barrierAndReturnReceiver();

    isLeftToTheRest.link(&jit);
#else
    UNUSED_PARAM(jit);
    UNUSED_PARAM(isOfSet);
#endif
}

static Vector<std::pair<Jump, unsigned>>& jumpsFromStubsToCallValue()
{
    static NeverDestroyed<Vector<std::pair<Jump, unsigned>>> jumps;
    return jumps.get();
}

static Vector<std::pair<Jump, unsigned>>& jumpsFromStubsToCallMethod()
{
    static NeverDestroyed<Vector<std::pair<Jump, unsigned>>> jumps;
    return jumps.get();
}

static void generateTableStub(CCallHelpers& jit, Stub stub, MapOrSetOperation operation, bool keyIsStringByType)
{
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    constexpr GPRReg receiver = IntrinsicRegisters::receiver;
    constexpr GPRReg key = IntrinsicRegisters::first;
    constexpr GPRReg value = IntrinsicRegisters::second;
    constexpr GPRReg found = calleeGPR;
    constexpr GPRReg thrown = T9;
    static_assert(noOverlap(receiver, key, value, found, thrown, instanceGPR));
    static_assert(noOverlap(found, thrown, GPRInfo::returnValueGPR, GPRInfo::returnValueGPR2));
    bool isOfSet = operation == MapOrSetOperation::SetHas || operation == MapOrSetOperation::SetAdd;
    bool isHas = operation == MapOrSetOperation::MapHas || operation == MapOrSetOperation::SetHas;
    TableMethod method = TableMethod::MapGet;
    switch (operation) {
    case MapOrSetOperation::MapGet:
        break;
    case MapOrSetOperation::MapHas:
        method = TableMethod::MapHas;
        break;
    case MapOrSetOperation::MapSet:
        method = TableMethod::MapSet;
        break;
    case MapOrSetOperation::SetHas:
        method = TableMethod::SetHas;
        break;
    case MapOrSetOperation::SetAdd:
        method = TableMethod::SetAdd;
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }

    jit.load32(Address(receiver, JSCell::structureIDOffset()), T9);
    Jump hasChanged = jit.branch32(CCallHelpers::NotEqual, T9, Address(instanceGPR, Instance::offsetOfReceiverStructureIDs() + static_cast<unsigned>(isOfSet ? Receiver::Set : Receiver::Map) * sizeof(uint32_t)));
    CCallHelpers::Label body = jit.label();
    stubLabels()[static_cast<unsigned>(stub)] = body;
    if (isHas || operation == MapOrSetOperation::MapGet)
        generateLookupInCompactTableWithoutFrame(jit, isOfSet, operation == MapOrSetOperation::MapGet, keyIsStringByType);
    else if (keyIsStringByType)
        generateStoreInCompactTableWithoutFrame(jit, isOfSet);
    generateMapOrSetOperation(jit, operation, nullptr, keyIsStringByType);

    hasChanged.link(&jit);
    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(4 * sizeof(EncodedJSValue)), sp);
    jit.store64(receiver, Address(sp));
    jit.store64(key, Address(sp, sizeof(EncodedJSValue)));
    jit.store64(value, Address(sp, 2 * sizeof(EncodedJSValue)));
    jit.move(receiver, A1);
    jit.move(TrustedImm32(static_cast<int32_t>(method)), A2);
    jit.move(instanceGPR, A0);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), operationGPR);
    jit.loadPtr(Address(operationGPR, static_cast<unsigned>(Entry::operationAOTTypedMethodOfTable) * sizeof(void*)), operationGPR);
    prepareCallOperation(jit, assertionScratchGPR);
    jit.call(operationGPR, OperationPtrTag);
    jit.move(GPRInfo::returnValueGPR, found);
    jit.move(GPRInfo::returnValueGPR2, thrown);
    jit.load64(Address(sp), receiver);
    jit.load64(Address(sp, sizeof(EncodedJSValue)), key);
    jit.load64(Address(sp, 2 * sizeof(EncodedJSValue)), value);
    jit.emitFunctionEpilogue();
    Jump hasThrown = jit.branchTestPtr(CCallHelpers::NonZero, thrown);
    jit.branchTestPtr(CCallHelpers::Zero, found).linkTo(body, &jit);
    if (isHas) {
        static_assert(noOverlap(A3, key, receiver) && A2 != receiver);
        jit.move(found, A3);
        jit.move(key, A2);
        jit.move(receiver, A1);
        jit.move(TrustedImm32(static_cast<unsigned>(Entry::operationAOTTypedHasByName) * sizeof(void*)), T9);
        jit.jump().linkTo(stubLabels()[static_cast<unsigned>(Stub::OperationValueWithInstance)], &jit);
    } else
        jumpsFromStubsToCallValue().append({ jit.jump(), operation == MapOrSetOperation::MapSet ? 2u : 1u });
    hasThrown.link(&jit);
    loadInstance(jit, T9);
    jumpToEntry(jit, T9, Entry::HandleException);
}
static void generateMapGetAskingReceiver(CCallHelpers&) { }
static void generateMapGet(CCallHelpers& jit) { generateTableStub(jit, Stub::MapGet, MapOrSetOperation::MapGet, false); }
static void generateMapHasAskingReceiver(CCallHelpers&) { }
static void generateMapHas(CCallHelpers& jit) { generateTableStub(jit, Stub::MapHas, MapOrSetOperation::MapHas, false); }
static void generateMapSetAskingReceiver(CCallHelpers&) { }
static void generateMapSet(CCallHelpers& jit) { generateTableStub(jit, Stub::MapSet, MapOrSetOperation::MapSet, false); }
static void generateSetHasAskingReceiver(CCallHelpers&) { }
static void generateSetHas(CCallHelpers& jit) { generateTableStub(jit, Stub::SetHas, MapOrSetOperation::SetHas, false); }
static void generateSetAddAskingReceiver(CCallHelpers&) { }
static void generateSetAdd(CCallHelpers& jit) { generateTableStub(jit, Stub::SetAdd, MapOrSetOperation::SetAdd, false); }
static void generateMapSetWithStringKeyAskingReceiver(CCallHelpers&) { }
static void generateMapSetWithStringKey(CCallHelpers& jit) { generateTableStub(jit, Stub::MapSetWithStringKey, MapOrSetOperation::MapSet, true); }
static void generateSetAddWithStringKeyAskingReceiver(CCallHelpers&) { }
static void generateSetAddWithStringKey(CCallHelpers& jit) { generateTableStub(jit, Stub::SetAddWithStringKey, MapOrSetOperation::SetAdd, true); }
static void generateMapGetWithStringKeyAskingReceiver(CCallHelpers&) { }
static void generateMapGetWithStringKey(CCallHelpers& jit) { generateTableStub(jit, Stub::MapGetWithStringKey, MapOrSetOperation::MapGet, true); }
static void generateMapHasWithStringKeyAskingReceiver(CCallHelpers&) { }
static void generateMapHasWithStringKey(CCallHelpers& jit) { generateTableStub(jit, Stub::MapHasWithStringKey, MapOrSetOperation::MapHas, true); }
static void generateSetHasWithStringKeyAskingReceiver(CCallHelpers&) { }
static void generateSetHasWithStringKey(CCallHelpers& jit) { generateTableStub(jit, Stub::SetHasWithStringKey, MapOrSetOperation::SetHas, true); }
static void generateMapDelete(CCallHelpers& jit) { generateMapOrSetOperation(jit, MapOrSetOperation::MapDelete, nullptr); }
static void generateSetDelete(CCallHelpers& jit) { generateMapOrSetOperation(jit, MapOrSetOperation::SetDelete, nullptr); }

static void generateLookupByBits(CCallHelpers& jit, MapOrSetOperation operation)
{
    using BaseIndex = CCallHelpers::BaseIndex;
    constexpr GPRReg key = IntrinsicRegisters::first;
    constexpr GPRReg value = IntrinsicRegisters::second;
    constexpr GPRReg result = IntrinsicRegisters::result;
#if CPU(ARM64)
    constexpr GPRReg table = IntrinsicRegisters::receiver;
    constexpr GPRReg hash = T13;
    constexpr GPRReg other = T14;
    constexpr GPRReg storage = T11;
    constexpr GPRReg data = T12;
    constexpr GPRReg index = T9;
    constexpr GPRReg entry = T10;
#else
    constexpr GPRReg table = X86Registers::r8;
    constexpr GPRReg hash = X86Registers::eax;
    constexpr GPRReg other = X86Registers::edx;
    constexpr GPRReg storage = X86Registers::ecx;
    constexpr GPRReg data = X86Registers::r9;
    constexpr GPRReg index = X86Registers::r10;
    constexpr GPRReg entry = other;
    static_assert(IntrinsicRegisters::receiver == hash);
#endif
    static_assert(noOverlap(table, key, value, hash, other, storage, data, index, instanceGPR));
    static_assert(noOverlap(table, key, value, hash, entry, storage, data, index, instanceGPR));
    static_assert(JSMap::Helper::capacityIndex() == JSSet::Helper::capacityIndex() && JSMap::Helper::hashTableStartIndex() == JSSet::Helper::hashTableStartIndex());
    bool isOfSet = operation == MapOrSetOperation::SetHas || operation == MapOrSetOperation::SetAdd;
    CCallHelpers::JumpList absent;
    auto yield = [&](JSValue yielded) {
        jit.move(CCallHelpers::TrustedImm64(JSValue::encode(yielded)), result);
        jit.ret();
    };
    auto yieldTable = [&] {
        jit.move(table, result);
        jit.ret();
    };
    auto goOnTo = [&](Entry function) {
        jit.move(TrustedImm32(static_cast<unsigned>(function) * sizeof(void*)), T9);
        jit.jump().linkTo(stubLabels()[static_cast<unsigned>(Stub::OperationValueWithInstance)], &jit);
    };

#if CPU(X86_64)
    jit.move(IntrinsicRegisters::receiver, table);
#endif
    jit.load64(Address(instanceGPR, Instance::offsetOfHashSecrets()), hash);
    jit.xor64(key, hash);
    jit.load64(Address(instanceGPR, Instance::offsetOfHashSecrets() + sizeof(uint64_t)), other);
    jit.xor64(key, other);
#if CPU(ARM64)
    jit.mul64(hash, other, index);
    jit.uMulHigh64(hash, other, hash);
    jit.xor64(index, hash);
#else
    jit.m_assembler.mulq_r(other);
    jit.xor64(other, hash);
#endif
    jit.loadPtr(Address(table, isOfSet ? JSSet::offsetOfStorage() : JSMap::offsetOfStorage()), storage);
    absent.append(jit.branchTestPtr(CCallHelpers::Zero, storage));
    jit.addPtr(TrustedImm32(JSCellButterfly::offsetOfData()), storage, data);
    jit.load32(Address(data, JSMap::Helper::capacityIndex() * sizeof(EncodedJSValue)), index);
    jit.sub32(TrustedImm32(1), index);
    jit.and32(hash, index);
    jit.add32(TrustedImm32(JSMap::Helper::hashTableStartIndex()), index);
    jit.load64(BaseIndex(data, index, CCallHelpers::TimesEight), index);
    absent.append(jit.branchTest64(CCallHelpers::Zero, index));
    CCallHelpers::Label loop = jit.label();
#if CPU(ARM64)
    jit.m_assembler.add<64>(entry, data, index, ARM64Assembler::UXTW, 3);
#else
    jit.zeroExtend32ToWord(index, index);
    jit.getEffectiveAddress(BaseIndex(data, index, CCallHelpers::TimesEight), entry);
#endif
    Jump found = jit.branch64(CCallHelpers::Equal, Address(entry), key);
    jit.load64(Address(entry, (isOfSet ? JSSet::Helper::ChainOffset : JSMap::Helper::ChainOffset) * sizeof(EncodedJSValue)), index);
    jit.branchTest64(CCallHelpers::NonZero, index).linkTo(loop, &jit);

    absent.link(&jit);
    switch (operation) {
    case MapOrSetOperation::MapGet:
        yield(jsUndefined());
        found.link(&jit);
        jit.load64(Address(entry, sizeof(EncodedJSValue)), result);
        jit.ret();
        return;
    case MapOrSetOperation::MapHas:
    case MapOrSetOperation::SetHas:
        yield(jsBoolean(false));
        found.link(&jit);
        yield(jsBoolean(true));
        return;
    case MapOrSetOperation::MapSet: {
        static_assert(noOverlap(A3, key, table, hash) && noOverlap(A2, table, hash) && A1 != hash);
        jit.move(value, A3);
        jit.move(key, A2);
        jit.move(table, A1);
        jit.move(hash, A4);
        goOnTo(Entry::operationAOTMapSet);
        found.link(&jit);
        jit.store64(value, Address(entry, sizeof(EncodedJSValue)));
        Jump isNoCell = jit.branchIfNotCell(value);
        jit.load8(Address(storage, JSCell::cellStateOffset()), index);
        Jump needsBarrier = jit.branch32(CCallHelpers::BelowOrEqual, index, Address(instanceGPR, Instance::offsetOfBarrierThreshold()));
        isNoCell.link(&jit);
        yieldTable();
        needsBarrier.link(&jit);
        jit.move(storage, T9);
        jit.move(table, result);
        jit.jump().linkTo(writeBarrierSlowCase(), &jit);
        return;
    }
    case MapOrSetOperation::SetAdd:
        static_assert(noOverlap(A2, table, hash) && A1 != hash);
        jit.move(key, A2);
        jit.move(table, A1);
        jit.move(hash, A3);
        goOnTo(Entry::operationAOTSetAdd);
        found.link(&jit);
        yieldTable();
        return;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}
static void generateMapGetByBits(CCallHelpers& jit) { generateLookupByBits(jit, MapOrSetOperation::MapGet); }
static void generateMapHasByBits(CCallHelpers& jit) { generateLookupByBits(jit, MapOrSetOperation::MapHas); }
static void generateMapSetByBits(CCallHelpers& jit) { generateLookupByBits(jit, MapOrSetOperation::MapSet); }
static void generateSetHasByBits(CCallHelpers& jit) { generateLookupByBits(jit, MapOrSetOperation::SetHas); }
static void generateSetAddByBits(CCallHelpers& jit) { generateLookupByBits(jit, MapOrSetOperation::SetAdd); }

enum class WeakLookup : uint8_t { Get, Has };

enum class ArraySearch : uint8_t { Includes, IndexOf };
enum class SearchedIn : uint8_t { Storage, Array, ArrayWithMethodRead };

static void generateArraySearch(CCallHelpers& jit, ArraySearch search, SearchedIn searchedIn = SearchedIn::Storage)
{
    constexpr GPRReg butterfly = A1;
    constexpr GPRReg needle = A2;
    constexpr GPRReg length = T9;
    constexpr GPRReg index = T10;
    constexpr GPRReg element = T11;
    constexpr GPRReg needleImpl = T12;
    constexpr GPRReg needleLength = T13;
    constexpr GPRReg impl = A3;
    constexpr GPRReg flags = R0;
    constexpr unsigned maxLengthToCompareOneByOne = 64;
    static_assert(noOverlap(butterfly, needle, length, index, element, needleImpl, needleLength, impl, flags));
    CCallHelpers::JumpList needsOperation;
    CCallHelpers::JumpList isAbsent;
    CCallHelpers::JumpList isFound;

    CCallHelpers::JumpList isLeftToCall;
    if (searchedIn != SearchedIn::Storage) {
        static_assert(butterfly == A1 && noOverlap(A1, A2, A3, A4, T9));
        jit.load8(Address(A1, JSCell::indexingTypeAndMiscOffset()), length);
        jit.and32(TrustedImm32(IndexingShapeMask), length);
        Jump keepsValues = jit.branch32(CCallHelpers::Equal, length, TrustedImm32(ContiguousShape));
        isLeftToCall.append(jit.branch32(CCallHelpers::NotEqual, length, TrustedImm32(Int32Shape)));
        keepsValues.link(&jit);
        jit.loadPtr(Address(A1, JSObject::butterflyOffset()), butterfly);
    }
    jit.load32(Address(butterfly, Butterfly::offsetOfPublicLength()), length);
    jit.move(TrustedImm32(0), index);
    isAbsent.append(jit.branchTest32(CCallHelpers::Zero, length));
    Jump isInt32 = jit.branchIfInt32(needle);
    needsOperation.append(jit.branchIfNumber(needle));
    Jump isCell = jit.branchIfCell(needle);
    static_assert(JSValue::ValueNull < JSValue::ValueUndefined && JSValue::ValueFalse < JSValue::ValueUndefined && JSValue::ValueTrue < JSValue::ValueUndefined);
    needsOperation.append(jit.branch64(search == ArraySearch::Includes ? CCallHelpers::AboveOrEqual : CCallHelpers::Above, needle, TrustedImm32(JSValue::ValueUndefined)));
    Jump isComparedByIdentity = jit.jump();
    isCell.link(&jit);
    jit.load8(Address(needle, JSCell::typeInfoTypeOffset()), element);
    Jump isObject = jit.branch32(CCallHelpers::AboveOrEqual, element, TrustedImm32(ObjectType));
    Jump isString = jit.branch32(CCallHelpers::Equal, element, TrustedImm32(StringType));
    needsOperation.append(jit.branch32(CCallHelpers::NotEqual, element, TrustedImm32(SymbolType)));
    isObject.link(&jit);
    isComparedByIdentity.link(&jit);
    needsOperation.append(jit.branch32(CCallHelpers::Above, length, TrustedImm32(maxLengthToCompareOneByOne)));
    CCallHelpers::Label next = jit.label();
    jit.load64(CCallHelpers::BaseIndex(butterfly, index, CCallHelpers::TimesEight), element);
    isFound.append(jit.branch64(CCallHelpers::Equal, element, needle));
    jit.add32(TrustedImm32(1), index);
    jit.branch32(CCallHelpers::NotEqual, index, length).linkTo(next, &jit);
    isAbsent.append(jit.jump());

    isString.link(&jit);
    jit.loadPtr(Address(needle, JSString::offsetOfValue()), needleImpl);
    needsOperation.append(jit.branchIfRopeStringImpl(needleImpl));
    jit.load32(Address(needleImpl, StringImpl::lengthMemoryOffset()), needleLength);
    CCallHelpers::Label nextForString = jit.label();
    CCallHelpers::JumpList differs;
    jit.load64(CCallHelpers::BaseIndex(butterfly, index, CCallHelpers::TimesEight), element);
    isFound.append(jit.branch64(CCallHelpers::Equal, element, needle));
    differs.append(jit.branchTest64(CCallHelpers::Zero, element));
    differs.append(jit.branchIfNotCell(element));
    differs.append(jit.branchIfNotType(element, StringType));
    jit.loadPtr(Address(element, JSString::offsetOfValue()), impl);
    needsOperation.append(jit.branchIfRopeStringImpl(impl));
    isFound.append(jit.branchPtr(CCallHelpers::Equal, impl, needleImpl));
    differs.append(jit.branch32(CCallHelpers::NotEqual, needleLength, Address(impl, StringImpl::lengthMemoryOffset())));
    jit.load32(Address(impl, StringImpl::flagsOffset()), flags);
    jit.and32(Address(needleImpl, StringImpl::flagsOffset()), flags);
    differs.append(jit.branchTest32(CCallHelpers::NonZero, flags, TrustedImm32(StringImpl::flagIsAtom())));
    // As long as each other, and not both atoms: the constants of a compiled program are none. By their characters.
    needsOperation.append(jit.branchTest32(CCallHelpers::Zero, flags, TrustedImm32(StringImpl::flagIs8Bit())));
    constexpr GPRReg characters = impl;
    constexpr GPRReg needleCharacters = needleImpl;
    constexpr GPRReg remaining = needleLength;
    CCallHelpers::JumpList differsInCharacters;
    jit.loadPtr(Address(impl, StringImpl::dataOffset()), characters);
    jit.loadPtr(Address(needleImpl, StringImpl::dataOffset()), needleCharacters);
    Jump fewerThanEight = jit.branch32(CCallHelpers::Below, remaining, TrustedImm32(8));
    CCallHelpers::Label wordLoop = jit.label();
    Jump isLastWord = jit.branch32(CCallHelpers::BelowOrEqual, remaining, TrustedImm32(8));
    jit.load64(Address(characters), flags);
    jit.load64(Address(needleCharacters), element);
    differsInCharacters.append(jit.branch64(CCallHelpers::NotEqual, flags, element));
    jit.add64(TrustedImm32(8), characters);
    jit.add64(TrustedImm32(8), needleCharacters);
    jit.sub32(TrustedImm32(8), remaining);
    jit.jump().linkTo(wordLoop, &jit);
    isLastWord.link(&jit);
    jit.add64(remaining, characters);
    jit.add64(remaining, needleCharacters);
    jit.load64(Address(characters, -8), flags);
    jit.load64(Address(needleCharacters, -8), element);
    isFound.append(jit.branch64(CCallHelpers::Equal, flags, element));
    differsInCharacters.append(jit.jump());
    fewerThanEight.link(&jit);
    CCallHelpers::Label byteLoop = jit.label();
    isFound.append(jit.branchTest32(CCallHelpers::Zero, remaining));
    jit.load8(Address(characters), flags);
    jit.load8(Address(needleCharacters), element);
    differsInCharacters.append(jit.branch32(CCallHelpers::NotEqual, flags, element));
    jit.add64(TrustedImm32(1), characters);
    jit.add64(TrustedImm32(1), needleCharacters);
    jit.sub32(TrustedImm32(1), remaining);
    jit.jump().linkTo(byteLoop, &jit);
    differsInCharacters.link(&jit);
    jit.loadPtr(Address(needle, JSString::offsetOfValue()), needleImpl);
    jit.load32(Address(needleImpl, StringImpl::lengthMemoryOffset()), needleLength);
    differs.link(&jit);
    jit.add32(TrustedImm32(1), index);
    jit.branch32(CCallHelpers::NotEqual, index, length).linkTo(nextForString, &jit);
    isAbsent.append(jit.jump());

    isInt32.link(&jit);
    CCallHelpers::Label nextForInt32 = jit.label();
    jit.load64(CCallHelpers::BaseIndex(butterfly, index, CCallHelpers::TimesEight), element);
    isFound.append(jit.branch64(CCallHelpers::Equal, element, needle));
    Jump isNotInt32 = jit.branchIfNotInt32(element);
    CCallHelpers::Label advance = jit.label();
    jit.add32(TrustedImm32(1), index);
    jit.branch32(CCallHelpers::NotEqual, index, length).linkTo(nextForInt32, &jit);

    isAbsent.link(&jit);
    jit.move(TrustedImm32(search == ArraySearch::Includes ? 0 : -1), R0);
    jit.ret();

    isFound.link(&jit);
    if (search == ArraySearch::Includes)
        jit.move(TrustedImm32(1), R0);
    else
        jit.move(index, R0);
    jit.ret();

    isNotInt32.link(&jit);
    jit.branchIfNotNumber(element).linkTo(advance, &jit);

    needsOperation.link(&jit);
    jit.move(TrustedImm32(0), A3);
    jit.move(TrustedImm32(static_cast<unsigned>(search == ArraySearch::Includes ? Entry::operationArrayIncludesValueInt32OrContiguous : Entry::operationArrayIndexOfValueInt32OrContiguous) * sizeof(void*)), T9);
    jit.jump().linkTo(stubLabels()[static_cast<unsigned>(Stub::OperationValueWithGlobalObject)], &jit);

    if (searchedIn == SearchedIn::Storage)
        return;
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    static_assert(noOverlap(A1, A2, thisGPR, calleeGPR, argumentGPR(0)) && calleeGPR != GPRInfo::returnValueGPR);
    static_assert(!((sizeof(CallerFrameAndPC) + 2 * sizeof(CPURegister)) % stackAlignmentBytes()));
    auto& builtins = Typed::Builtins::singleton();
    std::optional<unsigned> cellOfFunction;
    for (unsigned i = 0; i < builtins.size(); ++i) {
        auto& implementation = builtins.at(Typed::BuiltinID(i)).implementation;
        if (implementation.kind == Typed::BuiltinImplementation::Inline && implementation.inlined == (search == ArraySearch::IndexOf ? Typed::InlineBuiltin::ArrayIndexOf : Typed::InlineBuiltin::ArrayIncludes))
            cellOfFunction = builtins.cellOf(Typed::BuiltinID(i));
    }
    RELEASE_ASSERT(cellOfFunction);

    isLeftToCall.link(&jit);
    Jump isLeftToOperation;
    if (searchedIn == SearchedIn::ArrayWithMethodRead)
        isLeftToOperation = jit.branchTest64(CCallHelpers::NonZero, A3);
    else {
        jit.load8(Address(A1, JSCell::typeInfoTypeOffset()), T9);
        isLeftToOperation = jit.branch32(CCallHelpers::NotEqual, T9, TrustedImm32(ArrayType));
    }
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfTypedBuiltins()), calleeGPR);
    jit.loadPtr(Address(calleeGPR, *cellOfFunction * sizeof(void*)), calleeGPR);
    Jump isNotFilled = jit.branchTestPtr(CCallHelpers::Zero, calleeGPR);
    jit.move(A1, thisGPR);
    jit.move(A2, argumentGPR(0));
    CCallHelpers::Label callsFunction = jit.label();
    callHostFunction(jit, 1);

    isNotFilled.link(&jit);
    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(2 * sizeof(CPURegister)), sp);
    jit.storePair64(A1, A2, Address(sp, 0));
    jit.move(TrustedImm32(*cellOfFunction), T9);
    callStubFromStub(jit, Stub::TypedBuiltinCell);
    jit.move(GPRInfo::returnValueGPR, calleeGPR);
    jit.loadPair64(Address(sp, 0), thisGPR, argumentGPR(0));
    jit.emitFunctionEpilogue();
    jit.jump().linkTo(callsFunction, &jit);

    isLeftToOperation.link(&jit);
    if (searchedIn == SearchedIn::Array)
        jit.move(TrustedImm32(0), A3);
    jit.move(TrustedImm32(search == ArraySearch::IndexOf), A4);
    jit.move(TrustedImm32(static_cast<unsigned>(Entry::operationAOTTypedArraySearchByCall) * sizeof(void*)), T9);
    jit.jump().linkTo(stubLabels()[static_cast<unsigned>(Stub::OperationValueWithInstance)], &jit);
}

static void generateArrayIncludes(CCallHelpers& jit) { generateArraySearch(jit, ArraySearch::Includes); }
static void generateArrayIndexOf(CCallHelpers& jit) { generateArraySearch(jit, ArraySearch::IndexOf); }
static void generateArrayIncludesOrCall(CCallHelpers& jit) { generateArraySearch(jit, ArraySearch::Includes, SearchedIn::Array); }
static void generateArrayIndexOfOrCall(CCallHelpers& jit) { generateArraySearch(jit, ArraySearch::IndexOf, SearchedIn::Array); }
static void generateArrayIncludesOrCallMethodRead(CCallHelpers& jit) { generateArraySearch(jit, ArraySearch::Includes, SearchedIn::ArrayWithMethodRead); }
static void generateArrayIndexOfOrCallMethodRead(CCallHelpers& jit) { generateArraySearch(jit, ArraySearch::IndexOf, SearchedIn::ArrayWithMethodRead); }

template<typename SetUp>
static void generateAheadOf(CCallHelpers& jit, Entry operation, const SetUp& setUpAndCall)
{
    constexpr unsigned room = 6 * sizeof(void*);
    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(room), CCallHelpers::stackPointerRegister);
    jit.storePair64(A0, A1, Address(CCallHelpers::stackPointerRegister, 0));
    jit.storePair64(A2, A3, Address(CCallHelpers::stackPointerRegister, 16));
    jit.storePair64(A4, A5, Address(CCallHelpers::stackPointerRegister, 32));
    setUpAndCall();
    Jump gaveUp = jit.branchTestPtr(CCallHelpers::Zero, GPRInfo::returnValueGPR);
    jit.move(TrustedImm32(0), GPRInfo::returnValueGPR2);
    jit.emitFunctionEpilogue();
    jit.ret();
    gaveUp.link(&jit);
    jit.loadPair64(Address(CCallHelpers::stackPointerRegister, 0), A0, A1);
    jit.loadPair64(Address(CCallHelpers::stackPointerRegister, 16), A2, A3);
    jit.loadPair64(Address(CCallHelpers::stackPointerRegister, 32), A4, A5);
    jit.emitFunctionEpilogue();
    jit.move(instanceGPR, T9);
    jumpToEntry(jit, T9, operation);
}

static void generateAheadOf(CCallHelpers& jit, Entry operation, Stub helper, unsigned helperArguments)
{
    generateAheadOf(jit, operation, [&] {
        for (unsigned i = 0; i < helperArguments; ++i)
            jit.move(GPRInfo::toArgumentRegister(i + 1), GPRInfo::toArgumentRegister(i));
        callStubFromStub(jit, helper);
    });
}

static void generateNewArrayWithFastPath(CCallHelpers& jit)
{
    generateAheadOf(jit, Entry::NewArraySlowPath, [&] {
        jit.move(A1, A0);
        jit.move(A2, A1);
        Jump areInt32 = jit.branch32(CCallHelpers::Equal, A3, TrustedImm32(ArrayWithInt32));
        callStubFromStub(jit, Stub::HelperNewArray);
        Jump done = jit.jump();
        areInt32.link(&jit);
        callStubFromStub(jit, Stub::HelperNewInt32Array);
        done.link(&jit);
    });
}

static void generateValueAddWithFastPath(CCallHelpers& jit) { generateAheadOf(jit, Entry::ValueAddSlowPath, Stub::HelperAddStrings, 2); }

static void storeValuesAndClear(CCallHelpers& jit, unsigned firstValueArgument, unsigned count, unsigned end, GPRReg base, size_t offset)
{
    auto at = [&](unsigned i) { return static_cast<int32_t>(offset + i * sizeof(EncodedJSValue)); };
#if CPU(ARM64)
    auto word = [&](unsigned i) { return i < count ? GPRInfo::toArgumentRegister(firstValueArgument + i) : ARM64Registers::zr; };
    unsigned i = 0;
    for (; i + 1 < end; i += 2)
        jit.storePair64(word(i), word(i + 1), base, TrustedImm32(at(i)));
    if (i < end)
        jit.store64(word(i), Address(base, at(i)));
#else
    for (unsigned i = 0; i < count; ++i)
        jit.store64(GPRInfo::toArgumentRegister(firstValueArgument + i), Address(base, at(i)));
    for (unsigned i = count; i < end; ++i)
        jit.store64(TrustedImm32(0), Address(base, at(i)));
#endif
}

static void fenceAfterAllocation(CCallHelpers& jit, GPRReg scratch)
{
#if CPU(X86_64)
    UNUSED_PARAM(jit);
    UNUSED_PARAM(scratch);
#else
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfVM()), scratch);
    Jump isNotNeeded = jit.branchTest8(CCallHelpers::Zero, Address(scratch, VM::offsetOfHeapMutatorShouldBeFenced()));
    jit.storeFence();
    isNotNeeded.link(&jit);
#endif
}

static void generateEmptyArrayAtSite(CCallHelpers& jit, bool isLeaf)
{
    static_assert(noOverlap(T9, T11, T12, instanceGPR));
    auto jumpTo = [&](Jump jump, Stub stub) {
        jump.linkTo(stubLabels()[static_cast<unsigned>(stub) + isLeaf], &jit);
    };
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfTypedStatics()), T11);
    jit.load32(CCallHelpers::BaseIndex(T11, T9, CCallHelpers::TimesOne), T12);
    jit.add32(TrustedImm32(1 << arraySiteBornShift), T12);
    jit.store32(T12, CCallHelpers::BaseIndex(T11, T9, CCallHelpers::TimesOne));
    Jump isDueForReview = jit.branchTest32(CCallHelpers::Zero, T12, TrustedImm32((arraySiteInterval - 1) << arraySiteBornShift));
    auto isCounted = jit.label();
    static_assert(arraySiteBornShift == 8);
    jit.zeroExtend8To32(T12, T9);
    jumpTo(jit.branchTest32(CCallHelpers::Zero, T9), Stub::ArrayLiteral0);
    jumpTo(jit.jump(), Stub::EmptyArrayWithRoom);

    isDueForReview.link(&jit);
    preservingRegistersOfCaller(jit, false, [&](unsigned) {
        jit.move(T9, A1);
        jit.move(instanceGPR, A0);
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T11);
        jit.loadPtr(Address(T11, static_cast<unsigned>(Entry::operationAOTTypedReviewVectorLengthHint) * sizeof(void*)), T11);
        jit.call(T11, OperationPtrTag);
    });
    jit.load32(CCallHelpers::BaseIndex(T11, T9, CCallHelpers::TimesOne), T12);
    jit.jump().linkTo(isCounted, &jit);
}
static void generateEmptyArrayAtSite(CCallHelpers& jit) { generateEmptyArrayAtSite(jit, false); }
#if CPU(ARM64)
static void generateLeafEmptyArrayAtSite(CCallHelpers& jit) { generateEmptyArrayAtSite(jit, true); }
#else
static void generateLeafEmptyArrayAtSite(CCallHelpers& jit) { jit.breakpoint(); }
#endif

template<typename Functor>
static void callWithValuesOnStack(CCallHelpers& jit, Entry operation, unsigned firstValueArgument, unsigned count, const Functor& placeLastArgument)
{
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(WTF::roundUpToMultipleOf<stackAlignmentBytes()>(count * sizeof(EncodedJSValue))), sp);
    for (unsigned i = 0; i < count; ++i)
        jit.store64(GPRInfo::toArgumentRegister(firstValueArgument + i), Address(sp, i * sizeof(EncodedJSValue)));
    placeLastArgument();
    jit.move(sp, A1);
    jit.move(TrustedImm32(count), A2);
    loadInstance(jit, A0);
    jit.loadPtr(Address(A0, Instance::offsetOfRuntimeTable()), operationGPR);
    jit.loadPtr(Address(operationGPR, static_cast<unsigned>(operation) * sizeof(void*)), operationGPR);
    prepareCallOperation(jit, assertionScratchGPR);
    jit.call(operationGPR, OperationPtrTag);
    jit.emitFunctionEpilogue();
    Jump exception = jit.branchTestPtr(CCallHelpers::NonZero, GPRInfo::returnValueGPR2);
    jit.ret();
    exception.link(&jit);
    loadInstance(jit, T9);
    jumpToEntry(jit, T9, Entry::HandleException);
}

static void generateNewObjectLiteral(CCallHelpers& jit, unsigned count, bool isByRow = false)
{
    if (count > maxLiteralPropertiesInRegisters) {
        jit.breakpoint();
        return;
    }
    constexpr GPRReg cache = A1;
    constexpr GPRReg object = GPRInfo::returnValueGPR;
    constexpr GPRReg allocator = T9;
#if CPU(ARM64)
    constexpr GPRReg scratch = T10;
    constexpr GPRReg header = T11;
    constexpr GPRReg places = T12;
    constexpr GPRReg typeInfo = T13;
    static_assert(noOverlap(cache, object, allocator, scratch, header, places, typeInfo, A2, A3, A4, A5));
#else
    constexpr GPRReg scratch = A0;
    constexpr GPRReg header = T13;
    constexpr GPRReg places = allocator;
    static_assert(noOverlap(cache, object, allocator, scratch, header, A2, A3, A4, A5));
#endif
    static_assert(JSObject::butterflyOffset() == sizeof(EncodedJSValue));
    static_assert(JSObject::offsetOfInlineStorage() == 2 * sizeof(EncodedJSValue));
    static_assert(!OBJECT_OFFSETOF(Slot, structureID) && OBJECT_OFFSETOF(Slot, offset) == sizeof(uint32_t) && OBJECT_OFFSETOF(Slot, pointer) == sizeof(uint64_t));
    unsigned end = WTF::roundUpToMultipleOf<2>(count);

    CCallHelpers::JumpList slowCases;
    constexpr uint64_t upperHalfOfHeaderOfRows = static_cast<uint64_t>(Instance::typeInfoBlobOfRows) << 32;
    static_assert(sizeof(Slot) == 16);
#if CPU(ARM64)
    if (isByRow) {
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfRows()), allocator);
        jit.addLeftShift64(allocator, cache, TrustedImm32(4), cache);
    }
    jit.loadPair32(cache, TrustedImm32(OBJECT_OFFSETOF(Slot, structureID)), header, places);
    slowCases.append(jit.branchTest32(CCallHelpers::Zero, header));
    if (isByRow) {
        jit.loadPtr(Address(cache, OBJECT_OFFSETOF(Slot, pointer)), allocator);
        jit.move(CCallHelpers::TrustedImm64(upperHalfOfHeaderOfRows), typeInfo);
    } else
        jit.loadPair64(cache, TrustedImm32(sizeof(Slot)), typeInfo, allocator);
    jit.emitAllocateWithNonNullAllocator(object, JITAllocator::variable(), allocator, scratch, slowCases, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
    jit.or64(typeInfo, header);
    jit.storePair64(header, ARM64Registers::zr, object, TrustedImm32(0));
#else
    if (isByRow) {
        jit.lshift64(TrustedImm32(4), cache);
        jit.add64(Address(instanceGPR, Instance::offsetOfRows()), cache);
    }
    jit.load32(Address(cache, OBJECT_OFFSETOF(Slot, structureID)), header);
    slowCases.append(jit.branchTest32(CCallHelpers::Zero, header));
    jit.loadPtr(Address(cache, (isByRow ? 0 : sizeof(Slot)) + OBJECT_OFFSETOF(Slot, pointer)), allocator);
    jit.emitAllocateWithNonNullAllocator(object, JITAllocator::variable(), allocator, scratch, slowCases, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
    if (isByRow) {
        jit.move(CCallHelpers::TrustedImm64(upperHalfOfHeaderOfRows), scratch);
        jit.or64(scratch, header);
    } else
        jit.or64(Address(cache, sizeof(Slot)), header);
    jit.store64(header, Address(object, 0));
    jit.store64(TrustedImm32(0), Address(object, JSObject::butterflyOffset()));
    jit.load32(Address(cache, OBJECT_OFFSETOF(Slot, offset)), places);
#endif
    storeValuesAndClear(jit, 2, count, end, object, JSObject::offsetOfInlineStorage());
    Jump isFull = jit.branchTest32(CCallHelpers::Zero, places);
    jit.add32(TrustedImm32(1), places);
    jit.and32(TrustedImm32(Slot::offsetMask & ~1u), places);
#if CPU(ARM64)
    Jump isCleared = jit.branch32(CCallHelpers::BelowOrEqual, places, TrustedImm32(end));
    jit.getEffectiveAddress(CCallHelpers::BaseIndex(object, places, CCallHelpers::TimesEight), places);
    GPRReg lastPairOfValues = object;
    if (end) {
        jit.addPtr(TrustedImm32(end * sizeof(EncodedJSValue)), object, scratch);
        lastPairOfValues = scratch;
    }
    auto clear = jit.label();
    jit.storePair64(ARM64Registers::zr, ARM64Registers::zr, CCallHelpers::PostIndexAddress(places, -2 * static_cast<int>(sizeof(EncodedJSValue))));
    jit.branchPtr(CCallHelpers::Above, places, lastPairOfValues).linkTo(clear, &jit);
#else
    auto clear = jit.label();
    Jump isCleared = jit.branch32(CCallHelpers::BelowOrEqual, places, TrustedImm32(end));
    jit.sub32(TrustedImm32(2), places);
    jit.store64(TrustedImm32(0), CCallHelpers::BaseIndex(object, places, CCallHelpers::TimesEight, JSObject::offsetOfInlineStorage()));
    jit.store64(TrustedImm32(0), CCallHelpers::BaseIndex(object, places, CCallHelpers::TimesEight, JSObject::offsetOfInlineStorage() + sizeof(EncodedJSValue)));
    jit.jump().linkTo(clear, &jit);
#endif
    isCleared.link(&jit);
    isFull.link(&jit);
    fenceAfterAllocation(jit, scratch);
    jit.ret();

    slowCases.link(&jit);
    callWithValuesOnStack(jit, isByRow ? Entry::operationAOTNewObjectOfRow : Entry::operationAOTNewObjectLiteral, 2, count, [&] {
        jit.move(cache, A3);
    });
}

static void generateNewArrayLiteral(CCallHelpers& jit, unsigned count, bool areInt32)
{
    constexpr GPRReg array = GPRInfo::returnValueGPR;
    constexpr GPRReg allocator = T9;
#if CPU(ARM64)
    constexpr GPRReg scratch = T10;
    constexpr GPRReg storage = T11;
#else
    constexpr GPRReg scratch = A0;
    constexpr GPRReg storage = A5;
#endif
    static_assert(noOverlap(array, allocator, scratch, storage, A1, A2, A3, A4));
    static_assert(maxArrayElementsInRegisters == 4);
    RELEASE_ASSERT(count && count <= maxArrayElementsInRegisters);
    unsigned vectorLength = std::max<unsigned>(count, BASE_CONTIGUOUS_VECTOR_LEN) | 1;
    size_t storageSize = sizeof(IndexingHeader) + vectorLength * sizeof(EncodedJSValue);
    IndexingType indexingType = areInt32 ? ArrayWithInt32 : ArrayWithContiguous;
    Address structureID(instanceGPR, areInt32 ? Instance::offsetOfNewArrayWithInt32StructureID() : Instance::offsetOfNewArrayWithContiguousStructureID());

    CCallHelpers::JumpList slowCases;
    slowCases.append(jit.branchTest32(CCallHelpers::Zero, structureID));
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfAuxiliarySpace()), allocator);
    jit.loadPtr(Address(allocator, CompleteSubspace::offsetOfAllocatorForSizeStep() + MarkedSpace::sizeClassToIndex(storageSize) * sizeof(Allocator)), allocator);
    slowCases.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    jit.emitAllocateWithNonNullAllocator(storage, JITAllocator::variable(), allocator, scratch, slowCases, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfArrayAllocator()), allocator);
    slowCases.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    jit.emitAllocateWithNonNullAllocator(array, JITAllocator::variable(), allocator, scratch, slowCases, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
    jit.store64(CCallHelpers::TrustedImm64(static_cast<uint64_t>(vectorLength) << 32 | count), Address(storage, 0));
    storeValuesAndClear(jit, 1, count, vectorLength, storage, sizeof(IndexingHeader));
    jit.addPtr(TrustedImm32(sizeof(IndexingHeader)), storage);
    jit.load32(structureID, scratch);
    jit.or64(CCallHelpers::TrustedImm64(static_cast<uint64_t>(TypeInfoBlob(indexingType, TypeInfo(ArrayType, JSArray::StructureFlags)).blob()) << 32), scratch);
    jit.storePair64(scratch, storage, array, TrustedImm32(0));
    fenceAfterAllocation(jit, scratch);
    jit.ret();

    slowCases.link(&jit);
    callWithValuesOnStack(jit, Entry::operationAOTNewArray, 1, count, [&] {
        jit.move(TrustedImm32(indexingType), A3);
    });
}

// For code compiled from types: the boxed elements from the second argument register on. As generateNewArrayLiteral(), also for none. On ARM64 every register is kept but the result and the temporaries.
static void generateArrayLiteral(CCallHelpers& jit, unsigned count, bool isLeaf, unsigned capacityOfEmpty = 0)
{
    constexpr GPRReg array = GPRInfo::returnValueGPR;
    constexpr GPRReg allocator = T9;
#if CPU(ARM64)
    constexpr GPRReg scratch = T11;
    constexpr GPRReg storage = T12;
#else
    constexpr GPRReg scratch = A0;
    constexpr GPRReg storage = A5;
#endif
    static_assert(noOverlap(array, allocator, scratch, storage, A1, A2, A3, A4));
    static_assert(maxArrayElementsInRegisters == 4);
    RELEASE_ASSERT(count <= maxArrayElementsInRegisters);
    static_assert(vectorLengthOfArrayLiteral(0) == (BASE_CONTIGUOUS_VECTOR_LEN_EMPTY | 1));
    RELEASE_ASSERT(!capacityOfEmpty || (!count && capacityOfEmpty & 1));
    unsigned vectorLength = capacityOfEmpty ? capacityOfEmpty : vectorLengthOfArrayLiteral(count);
    unsigned countAndCapacity = count | capacityOfEmpty << capacityShiftInArrayLiteralCount;
    size_t storageSize = sizeof(IndexingHeader) + vectorLength * sizeof(EncodedJSValue);
    Address structureID(instanceGPR, Instance::offsetOfNewArrayWithContiguousStructureID());

    CCallHelpers::JumpList slowCases;
    // Zero once arrays are no longer allocated with this structure.
    slowCases.append(jit.branchTest32(CCallHelpers::Zero, structureID));
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfAuxiliarySpace()), allocator);
    jit.loadPtr(Address(allocator, CompleteSubspace::offsetOfAllocatorForSizeStep() + MarkedSpace::sizeClassToIndex(storageSize) * sizeof(Allocator)), allocator);
    slowCases.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    jit.emitAllocateWithNonNullAllocator(storage, JITAllocator::variable(), allocator, scratch, slowCases, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfArrayAllocator()), allocator);
    jit.emitAllocateWithNonNullAllocator(array, JITAllocator::variable(), allocator, scratch, slowCases, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
    jit.store64(CCallHelpers::TrustedImm64(static_cast<uint64_t>(vectorLength) << 32 | count), Address(storage, 0));
    storeValuesAndClear(jit, 1, count, vectorLength, storage, sizeof(IndexingHeader));
    jit.addPtr(TrustedImm32(sizeof(IndexingHeader)), storage);
    jit.load64(Address(instanceGPR, Instance::offsetOfNewArrayHeader(ArrayWithContiguous)), scratch);
    jit.storePair64(scratch, storage, array, TrustedImm32(0));
    fenceAfterAllocation(jit, scratch);
    returnToTypedCaller(jit, isLeaf);

    slowCases.link(&jit);
#if CPU(ARM64)
    jit.move(TrustedImm32(static_cast<unsigned>(Entry::operationAOTTypedNewArray) * sizeof(void*)), T9);
    generateColdOperation(jit, isLeaf, true, true, true, false, true, countAndCapacity);
#else
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    generateColdOperationPlacingArguments(jit, Entry::operationAOTTypedNewArray, std::max(count, 1u), [&] {
        for (unsigned i = 0; i < count; ++i)
            jit.store64(GPRInfo::toArgumentRegister(1 + i), Address(sp, i * sizeof(EncodedJSValue)));
        jit.move(sp, A2);
        jit.move(TrustedImm32(countAndCapacity), A1);
    });
#endif
}
#if CPU(ARM64)
#define AOT_GENERATE_ARRAY_LITERAL(n) \
static void generateArrayLiteral##n(CCallHelpers& jit) { generateArrayLiteral(jit, n, false); } \
static void generateLeafArrayLiteral##n(CCallHelpers& jit) { generateArrayLiteral(jit, n, true); }
#else
#define AOT_GENERATE_ARRAY_LITERAL(n) \
static void generateArrayLiteral##n(CCallHelpers& jit) { generateArrayLiteral(jit, n, false); } \
static void generateLeafArrayLiteral##n(CCallHelpers& jit) { jit.breakpoint(); }
#endif
AOT_GENERATE_ARRAY_LITERAL(0)
AOT_GENERATE_ARRAY_LITERAL(1)
AOT_GENERATE_ARRAY_LITERAL(2)
AOT_GENERATE_ARRAY_LITERAL(3)
AOT_GENERATE_ARRAY_LITERAL(4)
#undef AOT_GENERATE_ARRAY_LITERAL

#if CPU(ARM64)
static void generateElementOrNothing(CCallHelpers& jit, bool isLeaf)
{
    constexpr GPRReg result = GPRInfo::returnValueGPR;
    constexpr GPRReg shape = T9;
    constexpr GPRReg butterfly = T11;
    constexpr GPRReg savedNumber = T12;
    constexpr GPRReg savedScratch = T13;
    constexpr FPRReg number = FPRInfo::fpRegT0;
    constexpr FPRReg scratch = FPRInfo::fpRegT1;
    static_assert(noOverlap(result, shape, T10, butterfly, savedNumber, savedScratch, A1, A2, A3));
    CCallHelpers::JumpList generally;
    CCallHelpers::JumpList absent;
    generally.append(jit.branchIfNotCell(A1));
    generally.append(jit.branch8(CCallHelpers::NotEqual, Address(A1, JSCell::typeInfoTypeOffset()), TrustedImm32(ArrayType)));
    jit.load8(Address(A1, JSCell::indexingTypeAndMiscOffset()), shape);
    jit.and32(TrustedImm32(IndexingShapeMask), shape);
    jit.loadPtr(Address(A1, JSObject::butterflyOffset()), butterfly);
    jit.load32(Address(butterfly, Butterfly::offsetOfPublicLength()), result);
    absent.append(jit.branch64(CCallHelpers::AboveOrEqual, A2, result));
    Jump keepsDoubles = jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(DoubleShape));
    absent.append(jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(UndecidedShape)));
    Jump keepsValues = jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(ContiguousShape));
    generally.append(jit.branch32(CCallHelpers::NotEqual, shape, TrustedImm32(Int32Shape)));
    keepsValues.link(&jit);
    jit.load64(CCallHelpers::BaseIndex(butterfly, A2, CCallHelpers::TimesEight), result);
    absent.append(jit.branchTest64(CCallHelpers::Zero, result));
    returnToTypedCaller(jit, isLeaf);

    keepsDoubles.link(&jit);
    jit.moveDoubleTo64(number, savedNumber);
    jit.moveDoubleTo64(scratch, savedScratch);
    jit.loadDouble(CCallHelpers::BaseIndex(butterfly, A2, CCallHelpers::TimesEight), number);
    Jump isHole = jit.branchIfNaN(number);
    CCallHelpers::JumpList isNoInt32;
    branchIfNoExactInt32(jit, number, result, isNoInt32, scratch);
    jit.add64(numberTag, result);
    Jump isBoxed = jit.jump();
    isNoInt32.link(&jit);
    jit.moveDoubleTo64(number, result);
    jit.sub64(numberTag, result);
    isBoxed.link(&jit);
    jit.move64ToDouble(savedNumber, number);
    jit.move64ToDouble(savedScratch, scratch);
    returnToTypedCaller(jit, isLeaf);

    isHole.link(&jit);
    jit.move64ToDouble(savedNumber, number);
    jit.move64ToDouble(savedScratch, scratch);
    absent.link(&jit);
    generally.append(jit.branch64(CCallHelpers::Above, A2, CCallHelpers::TrustedImm64(MAX_ARRAY_INDEX)));
    jit.move(A3, result);
    returnToTypedCaller(jit, isLeaf);

    generally.link(&jit);
    jit.move(TrustedImm32(static_cast<unsigned>(Entry::operationAOTTypedElementOrNothing) * sizeof(void*)), T9);
    generateColdOperation(jit, isLeaf, true);
}
static void generateElementOrNothing(CCallHelpers& jit) { generateElementOrNothing(jit, false); }
static void generateLeafElementOrNothing(CCallHelpers& jit) { generateElementOrNothing(jit, true); }
#else
static void generateElementOrNothing(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafElementOrNothing(CCallHelpers& jit) { jit.breakpoint(); }
#endif
static void generateTypedNewPromise(CCallHelpers& jit, bool isLeaf)
{
    constexpr GPRReg promise = GPRInfo::returnValueGPR;
    constexpr GPRReg allocator = T9;
    constexpr GPRReg scratch = T11;
    static_assert(noOverlap(promise, allocator, T10, scratch));
    static_assert(JSPromise::offsetOfSlot() == JSPromise::offsetOfPacked() + static_cast<ptrdiff_t>(sizeof(uint64_t)));
    RELEASE_ASSERT(JSObject::butterflyOffset() == sizeof(uint64_t) && !JSValue::encode(JSValue()));
    CCallHelpers::JumpList slowCases;
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfAllocatorFor(Instance::InlineAllocation::Promise)), allocator);
    slowCases.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    jit.emitAllocateWithNonNullAllocator(promise, JITAllocator::variable(), allocator, scratch, slowCases, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
    jit.load64(Address(instanceGPR, Instance::offsetOfHeaderFor(Instance::InlineAllocation::Promise)), scratch);
#if CPU(ARM64)
    jit.storePair64(scratch, ARM64Registers::zr, promise, TrustedImm32(0));
    jit.storePair64(ARM64Registers::zr, ARM64Registers::zr, promise, TrustedImm32(JSPromise::offsetOfPacked()));
#else
    jit.store64(scratch, Address(promise));
    jit.store64(TrustedImm32(0), Address(promise, JSObject::butterflyOffset()));
    jit.store64(TrustedImm32(0), Address(promise, JSPromise::offsetOfPacked()));
    jit.store64(TrustedImm32(0), Address(promise, JSPromise::offsetOfSlot()));
#endif
    fenceAfterAllocation(jit, scratch);
    returnToTypedCaller(jit, isLeaf);

    slowCases.link(&jit);
    jit.move(TrustedImm32(static_cast<unsigned>(Entry::operationAOTTypedNewPromise) * sizeof(void*)), T9);
    generateColdOperation(jit, isLeaf, true, false);
}
static void generateTypedNewPromise(CCallHelpers& jit) { generateTypedNewPromise(jit, false); }
#if CPU(ARM64)
static void generateLeafTypedNewPromise(CCallHelpers& jit) { generateTypedNewPromise(jit, true); }
#else
static void generateLeafTypedNewPromise(CCallHelpers& jit) { jit.breakpoint(); }
#endif

static void generateAwaitMayContinue(CCallHelpers& jit, bool isLeaf)
{
    constexpr GPRReg result = GPRInfo::returnValueGPR;
    constexpr GPRReg awaited = A1;
    constexpr GPRReg mode = A2;
    constexpr GPRReg word = T9;
    constexpr GPRReg other = T11;
    constexpr unsigned flagsShift = CompactPointerTuple<JSCell*, uint16_t>::maxNumberOfBitsInPointer;
    static_assert(noOverlap(result, awaited, mode, word, T10, other));
    static_assert(!static_cast<unsigned>(JSPromise::Status::Pending) && static_cast<unsigned>(JSPromise::Status::Fulfilled) == 1 && static_cast<unsigned>(JSPromise::Status::Rejected) == 2);
    CCallHelpers::JumpList mustSuspend;
    mustSuspend.append(jit.branchTest32(CCallHelpers::Zero, mode, TrustedImm32(Typed::CoroutineFrame::isEnteredByJobLoop)));
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfMicrotaskQueue()), word);
    jit.load64(Address(word, MicrotaskQueue::offsetOfStart()), other);
    jit.load64(Address(word, MicrotaskQueue::offsetOfEnd()), word);
    mustSuspend.append(jit.branch64(CCallHelpers::NotEqual, other, word));
    jit.load8(Address(instanceGPR, Instance::offsetOfAwaitMustSuspend()), word);
    mustSuspend.append(jit.branchTest32(CCallHelpers::NonZero, word));
    jit.move(awaited, result);
    Jump isNoCell = jit.branchIfNotCell(awaited);
    Jump isNoObject = jit.branchIfNotObject(awaited);
    jit.load32(Address(awaited, JSCell::structureIDOffset()), word);
    jit.load32(Address(instanceGPR, Instance::offsetOfHeaderFor(Instance::InlineAllocation::Promise)), other);
    mustSuspend.append(jit.branch32(CCallHelpers::NotEqual, word, other));
    jit.load64(Address(awaited, JSPromise::offsetOfPacked()), word);
    jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(JSPromise::Status::Fulfilled) << flagsShift), other);
    mustSuspend.append(jit.branchTest64(CCallHelpers::Zero, word, other));
    jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(JSPromise::isHandledFlag) << flagsShift), other);
    jit.or64(other, word);
    jit.store64(word, Address(awaited, JSPromise::offsetOfPacked()));
    jit.load64(Address(awaited, JSPromise::offsetOfSlot()), result);
    isNoCell.link(&jit);
    isNoObject.link(&jit);
    returnToTypedCaller(jit, isLeaf);

    mustSuspend.link(&jit);
    jit.move(TrustedImm32(0), result);
    returnToTypedCaller(jit, isLeaf);
}
static void generateAwaitMayContinue(CCallHelpers& jit) { generateAwaitMayContinue(jit, false); }
#if CPU(ARM64)
static void generateLeafAwaitMayContinue(CCallHelpers& jit) { generateAwaitMayContinue(jit, true); }
#else
static void generateLeafAwaitMayContinue(CCallHelpers& jit) { jit.breakpoint(); }
#endif

static void generateEmptyArrayWithRoomFor1(CCallHelpers& jit) { generateArrayLiteral(jit, 0, false, 1); }
static void generateEmptyArrayWithRoomFor3(CCallHelpers& jit) { generateArrayLiteral(jit, 0, false, 3); }
#if CPU(ARM64)
static void generateLeafEmptyArrayWithRoomFor1(CCallHelpers& jit) { generateArrayLiteral(jit, 0, true, 1); }
static void generateLeafEmptyArrayWithRoomFor3(CCallHelpers& jit) { generateArrayLiteral(jit, 0, true, 3); }
#else
static void generateLeafEmptyArrayWithRoomFor1(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafEmptyArrayWithRoomFor3(CCallHelpers& jit) { jit.breakpoint(); }
#endif

// For code compiled from types. T9: see capacityShiftInArrayOfNumbers. Yields an array that keeps its elements that way, with holes from the length on: the caller stores the elements, which no collector looks at.
// 0: arrays are no longer made with their original structures. Where a free list has run out the operation makes the same, of the same sizes, so that it is those lists that are filled. On ARM64 every register is kept but the result and the temporaries. `capacityOfEmpty`: it is handed nothing: an empty one with that much room.
static void generateArrayOfNumbers(CCallHelpers& jit, IndexingType indexingType, bool isLeaf, unsigned capacityOfEmpty = 0, bool takesRoomAlone = false)
{
    constexpr GPRReg array = GPRInfo::returnValueGPR;
    constexpr GPRReg allocator = T9;
#if CPU(ARM64)
    constexpr GPRReg scratch = T11;
    constexpr GPRReg storage = T12;
    constexpr GPRReg capacity = T13;
    constexpr GPRReg length = T14;
    static_assert(noOverlap(array, allocator, scratch, storage, capacity, length));
#else
    constexpr GPRReg scratch = A0;
    constexpr GPRReg storage = A5;
    constexpr GPRReg capacity = T13;
    constexpr GPRReg length = T14;
    static_assert(noOverlap(array, allocator, scratch, storage, capacity, length));
#endif
    static_assert(sizeof(IndexingHeader) == sizeof(EncodedJSValue) && MarkedSpace::sizeStep == 2 * sizeof(EncodedJSValue) && sizeof(Allocator) == sizeof(void*));
    static_assert(sizeof(IndexingHeader) + maxCapacityOfArrayOfNumbers * sizeof(EncodedJSValue) <= MarkedSpace::largeCutoff && maxCapacityOfArrayOfNumbers & 1);
    RELEASE_ASSERT(indexingType == ArrayWithDouble || indexingType == ArrayWithInt32 || indexingType == ArrayWithContiguous);
    if (takesRoomAlone) {
        static_assert(vectorLengthOfArrayLiteral(0) == 5);
        auto jumpTo = [&](Jump jump, Stub stub) {
            jump.linkTo(stubLabels()[static_cast<unsigned>(stub) + isLeaf], &jit);
        };
        jumpTo(jit.branch64(CCallHelpers::BelowOrEqual, T9, TrustedImm32(1)), Stub::EmptyArrayWithRoomFor1);
        jumpTo(jit.branch64(CCallHelpers::BelowOrEqual, T9, TrustedImm32(3)), Stub::EmptyArrayWithRoomFor3);
        jumpTo(jit.branch64(CCallHelpers::BelowOrEqual, T9, TrustedImm32(vectorLengthOfArrayLiteral(0))), Stub::ArrayLiteral0);
        jumpTo(jit.branch64(CCallHelpers::Above, T9, TrustedImm32(maxCapacityOfArrayOfNumbers)), Stub::ArrayLiteral0);
        jumpTo(jit.branchTest32(CCallHelpers::Zero, Address(instanceGPR, Instance::offsetOfNewArrayWithContiguousStructureID())), Stub::ArrayLiteral0);
        jit.or32(TrustedImm32(1), T9, capacity);
        jit.move(TrustedImm32(0), length);
    }

    CCallHelpers::JumpList failed;
    // Zero once arrays are no longer allocated with their original structures.
    CCallHelpers::JumpList isNotToBeHad;
    if (!takesRoomAlone)
        isNotToBeHad.append(jit.branchTest32(CCallHelpers::Zero, Address(instanceGPR, Instance::offsetOfNewArrayWithContiguousStructureID())));
    bool isEmpty = capacityOfEmpty;
    RELEASE_ASSERT(!isEmpty || capacityOfEmpty & 1);
    if (isEmpty) {
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfAuxiliarySpace()), allocator);
        jit.loadPtr(Address(allocator, CompleteSubspace::offsetOfAllocatorForSizeStep() + MarkedSpace::sizeClassToIndex(sizeof(IndexingHeader) + capacityOfEmpty * sizeof(EncodedJSValue)) * sizeof(Allocator)), allocator);
    } else {
        if (!takesRoomAlone) {
            jit.and32(TrustedImm32((1 << capacityShiftInArrayOfNumbers) - 1), T9, length);
            jit.urshift32(T9, TrustedImm32(capacityShiftInArrayOfNumbers), capacity);
            failed.append(jit.branch32(CCallHelpers::Above, capacity, TrustedImm32(maxCapacityOfArrayOfNumbers)));
        }
        // With the header an even number of words.
        jit.add32(TrustedImm32(1), capacity, scratch);
        jit.urshift32(TrustedImm32(1), scratch);
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfAuxiliarySpace()), allocator);
        jit.loadPtr(CCallHelpers::BaseIndex(allocator, scratch, CCallHelpers::TimesEight, CompleteSubspace::offsetOfAllocatorForSizeStep()), allocator);
    }
    failed.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    jit.emitAllocateWithNonNullAllocator(storage, JITAllocator::variable(), allocator, scratch, failed, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfArrayAllocator()), allocator);
    jit.emitAllocateWithNonNullAllocator(array, JITAllocator::variable(), allocator, scratch, failed, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
    CCallHelpers::TrustedImm64 hole(indexingType == ArrayWithDouble ? std::bit_cast<int64_t>(PNaN) : JSValue::encode(JSValue()));
    if (isEmpty) {
        jit.store64(CCallHelpers::TrustedImm64(static_cast<uint64_t>(capacityOfEmpty) << 32), Address(storage, 0));
        jit.addPtr(TrustedImm32(sizeof(IndexingHeader)), storage);
        jit.move(hole, scratch);
        for (unsigned i = 0; i + 1 < capacityOfEmpty; i += 2)
            jit.storePair64(scratch, scratch, storage, TrustedImm32(i * sizeof(EncodedJSValue)));
        jit.store64(scratch, Address(storage, (capacityOfEmpty - 1) * sizeof(EncodedJSValue)));
    } else {
        jit.store32(length, Address(storage, 0));
        jit.store32(capacity, Address(storage, sizeof(uint32_t)));
        jit.addPtr(TrustedImm32(sizeof(IndexingHeader)), storage);
        if (takesRoomAlone) {
            static_assert(vectorLengthOfArrayLiteral(0) >= 2);
            jit.move(hole, scratch);
            jit.getEffectiveAddress(CCallHelpers::BaseIndex(storage, capacity, CCallHelpers::TimesEight), capacity);
            jit.store64(scratch, Address(storage));
            jit.addPtr(TrustedImm32(sizeof(EncodedJSValue)), storage, length);
            CCallHelpers::Label fillByPairs = jit.label();
#if CPU(ARM64)
            jit.storePair64(scratch, scratch, CCallHelpers::PostIndexAddress(length, 2 * sizeof(EncodedJSValue)));
#else
            jit.storePair64(scratch, scratch, length, TrustedImm32(0));
            jit.addPtr(TrustedImm32(2 * sizeof(EncodedJSValue)), length);
#endif
            jit.branchPtr(CCallHelpers::Below, length, capacity).linkTo(fillByPairs, &jit);
        } else {
            Jump isFull = jit.branch32(CCallHelpers::AboveOrEqual, length, capacity);
            jit.move(hole, scratch);
            CCallHelpers::Label fill = jit.label();
            jit.store64(scratch, CCallHelpers::BaseIndex(storage, length, CCallHelpers::TimesEight));
            jit.add32(TrustedImm32(1), length);
            jit.branch32(CCallHelpers::Below, length, capacity).linkTo(fill, &jit);
            isFull.link(&jit);
        }
    }
    jit.load64(Address(instanceGPR, Instance::offsetOfNewArrayHeader(indexingType)), scratch);
    jit.storePair64(scratch, storage, array, TrustedImm32(0));
    fenceAfterAllocation(jit, scratch);
    returnToTypedCaller(jit, isLeaf);

    isNotToBeHad.link(&jit);
    jit.move(TrustedImm32(0), array);
    returnToTypedCaller(jit, isLeaf);

    failed.link(&jit);
#if CPU(ARM64)
    constexpr GPRReg lengthAndCapacity = T12;
    constexpr GPRReg kept = T13;
    if (isEmpty)
        jit.move(TrustedImm32(capacityOfEmpty << capacityShiftInArrayOfNumbers), lengthAndCapacity);
    else {
        jit.lshift32(capacity, TrustedImm32(capacityShiftInArrayOfNumbers), lengthAndCapacity);
        jit.or32(length, lengthAndCapacity);
    }
    jit.move(TrustedImm32(indexingType), kept);
    jit.move(TrustedImm32(static_cast<unsigned>(Entry::operationAOTTypedNewArrayOfNumbers) * sizeof(void*)), T9);
    generateColdOperation(jit, isLeaf, true, true, true, false, true, std::nullopt, lengthAndCapacity, kept);
#else
    generateColdOperationPlacingArguments(jit, Entry::operationAOTTypedNewArrayOfNumbers, 0, [&] {
        if (isEmpty)
            jit.move(TrustedImm32(capacityOfEmpty << capacityShiftInArrayOfNumbers), A1);
        else {
            jit.move(capacity, A1);
            jit.lshift32(TrustedImm32(capacityShiftInArrayOfNumbers), A1);
            jit.or32(length, A1);
        }
        jit.move(TrustedImm32(indexingType), A2);
    });
#endif
}
static void generateArrayOfDoubles(CCallHelpers& jit) { generateArrayOfNumbers(jit, ArrayWithDouble, false); }
static void generateArrayOfInt32s(CCallHelpers& jit) { generateArrayOfNumbers(jit, ArrayWithInt32, false); }
static void generateEmptyArrayWithRoom(CCallHelpers& jit) { generateArrayOfNumbers(jit, ArrayWithContiguous, false, 0, true); }
static void generateArrayOfValues(CCallHelpers& jit) { generateArrayOfNumbers(jit, ArrayWithContiguous, false); }
static void generateEmptyArrayOfDoubles(CCallHelpers& jit) { generateArrayOfNumbers(jit, ArrayWithDouble, false, vectorLengthOfArrayLiteral(0)); }
static void generateEmptyArrayOfInt32s(CCallHelpers& jit) { generateArrayOfNumbers(jit, ArrayWithInt32, false, vectorLengthOfArrayLiteral(0)); }
static void generateEmptyArrayOfDoublesWithRoomFor1(CCallHelpers& jit) { generateArrayOfNumbers(jit, ArrayWithDouble, false, 1); }
static void generateEmptyArrayOfDoublesWithRoomFor3(CCallHelpers& jit) { generateArrayOfNumbers(jit, ArrayWithDouble, false, 3); }
#if CPU(ARM64)
static void generateLeafArrayOfDoubles(CCallHelpers& jit) { generateArrayOfNumbers(jit, ArrayWithDouble, true); }
static void generateLeafArrayOfInt32s(CCallHelpers& jit) { generateArrayOfNumbers(jit, ArrayWithInt32, true); }
static void generateLeafEmptyArrayWithRoom(CCallHelpers& jit) { generateArrayOfNumbers(jit, ArrayWithContiguous, true, 0, true); }
static void generateLeafArrayOfValues(CCallHelpers& jit) { generateArrayOfNumbers(jit, ArrayWithContiguous, true); }
static void generateLeafEmptyArrayOfDoubles(CCallHelpers& jit) { generateArrayOfNumbers(jit, ArrayWithDouble, true, vectorLengthOfArrayLiteral(0)); }
static void generateLeafEmptyArrayOfInt32s(CCallHelpers& jit) { generateArrayOfNumbers(jit, ArrayWithInt32, true, vectorLengthOfArrayLiteral(0)); }
static void generateLeafEmptyArrayOfDoublesWithRoomFor1(CCallHelpers& jit) { generateArrayOfNumbers(jit, ArrayWithDouble, true, 1); }
static void generateLeafEmptyArrayOfDoublesWithRoomFor3(CCallHelpers& jit) { generateArrayOfNumbers(jit, ArrayWithDouble, true, 3); }
#else
static void generateLeafArrayOfDoubles(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafArrayOfInt32s(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafEmptyArrayWithRoom(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafArrayOfValues(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafEmptyArrayOfDoubles(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafEmptyArrayOfInt32s(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafEmptyArrayOfDoublesWithRoomFor1(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafEmptyArrayOfDoublesWithRoomFor3(CCallHelpers& jit) { jit.breakpoint(); }
#endif

static void generateArraySlice(CCallHelpers& jit, bool isWhole)
{
    constexpr bool isLeaf = false;
    constexpr GPRReg result = GPRInfo::returnValueGPR;
    constexpr GPRReg shape = T9;
#if CPU(ARM64)
    constexpr GPRReg source = T11;
    constexpr GPRReg count = T12;
    constexpr GPRReg storage = T13;
    constexpr GPRReg scratch = T14;
#else
    constexpr GPRReg source = T10;
    constexpr GPRReg count = T11;
    constexpr GPRReg storage = T12;
    constexpr GPRReg scratch = T13;
#endif
    constexpr GPRReg length = result;
    constexpr GPRReg allocatorOfStorage = result;
    constexpr GPRReg vectorLength = result;
    constexpr GPRReg index = result;
    constexpr GPRReg from = storage;
    constexpr GPRReg allocatorOfCell = source;
    constexpr unsigned maxCount = 64;
    static_assert(noOverlap(result, shape, source, count, storage, scratch, A1, A2, A3));
    static_assert(Int32Shape + 2 == DoubleShape && DoubleShape + 2 == ContiguousShape);
    static_assert(sizeof(IndexingHeader) == sizeof(EncodedJSValue) && MarkedSpace::sizeStep == 2 * sizeof(EncodedJSValue) && sizeof(Allocator) == sizeof(void*));
    static_assert(Butterfly::offsetOfPublicLength() == -static_cast<ptrdiff_t>(sizeof(IndexingHeader)) && Butterfly::offsetOfVectorLength() == Butterfly::offsetOfPublicLength() + static_cast<ptrdiff_t>(sizeof(uint32_t)));
    static_assert(Instance::arrayKindShift == 1);

    CCallHelpers::JumpList fails;
    jit.load8(Address(A1, JSCell::indexingTypeAndMiscOffset()), shape);
    jit.and32(TrustedImm32(IndexingShapeMask), shape);
    jit.move(shape, scratch);
    jit.sub32(TrustedImm32(Int32Shape), scratch);
    fails.append(jit.branch32(CCallHelpers::Above, scratch, TrustedImm32(ContiguousShape - Int32Shape)));
    jit.loadPtr(Address(A1, JSObject::butterflyOffset()), source);
    if (isWhole)
        jit.load32(Address(source, Butterfly::offsetOfPublicLength()), count);
    else {
        jit.load32(Address(source, Butterfly::offsetOfPublicLength()), length);
        auto clamp = [&](GPRReg given, GPRReg clamped) {
            jit.move(given, clamped);
            Jump isNotNegative = jit.branch64(CCallHelpers::GreaterThanOrEqual, clamped, TrustedImm32(0));
            jit.add64(length, clamped);
            Jump isFromEnd = jit.branch64(CCallHelpers::GreaterThanOrEqual, clamped, TrustedImm32(0));
            jit.move(TrustedImm32(0), clamped);
            Jump isStart = jit.jump();
            isNotNegative.link(&jit);
            Jump isWithin = jit.branch64(CCallHelpers::LessThanOrEqual, clamped, length);
            jit.move(length, clamped);
            isWithin.link(&jit);
            isFromEnd.link(&jit);
            isStart.link(&jit);
        };
        clamp(A2, from);
        clamp(A3, count);
        jit.sub64(from, count);
        Jump hasElements = jit.branch64(CCallHelpers::GreaterThan, count, TrustedImm32(0));
        jit.move(TrustedImm32(0), count);
        hasElements.link(&jit);
        jit.getEffectiveAddress(CCallHelpers::BaseIndex(source, from, CCallHelpers::TimesEight), source);
    }
    fails.append(jit.branch32(CCallHelpers::Above, count, TrustedImm32(maxCount)));
    fails.append(jit.branchTest32(CCallHelpers::Zero, Address(instanceGPR, Instance::offsetOfNewArrayWithContiguousStructureID())));
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfAuxiliarySpace()), allocatorOfStorage);
    Jump isOne = jit.branch32(CCallHelpers::Equal, count, TrustedImm32(1));

    jit.or32(TrustedImm32(1), count, scratch);
    jit.add32(TrustedImm32(1), scratch);
    jit.urshift32(TrustedImm32(1), scratch);
    jit.loadPtr(CCallHelpers::BaseIndex(allocatorOfStorage, scratch, CCallHelpers::TimesEight, CompleteSubspace::offsetOfAllocatorForSizeStep()), allocatorOfStorage);
    fails.append(jit.branchTestPtr(CCallHelpers::Zero, allocatorOfStorage));
    jit.emitAllocateWithNonNullAllocator(storage, JITAllocator::variable(), allocatorOfStorage, scratch, fails, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
    jit.or32(TrustedImm32(1), count, vectorLength);
    jit.store32(count, Address(storage, 0));
    jit.store32(vectorLength, Address(storage, sizeof(uint32_t)));
    jit.move(TrustedImm32(0), scratch);
    Jump keepsValues = jit.branch32(CCallHelpers::NotEqual, shape, TrustedImm32(DoubleShape));
    jit.move(CCallHelpers::TrustedImm64(std::bit_cast<int64_t>(PNaN)), scratch);
    keepsValues.link(&jit);
    jit.store64(scratch, CCallHelpers::BaseIndex(storage, vectorLength, CCallHelpers::TimesEight));
    jit.move(TrustedImm32(0), index);
    Jump isCopied = jit.branchTest32(CCallHelpers::Zero, count);
    CCallHelpers::Label copy = jit.label();
    jit.load64(CCallHelpers::BaseIndex(source, index, CCallHelpers::TimesEight), scratch);
    jit.store64(scratch, CCallHelpers::BaseIndex(storage, index, CCallHelpers::TimesEight, sizeof(IndexingHeader)));
    jit.add32(TrustedImm32(1), index);
    jit.branch32(CCallHelpers::Below, index, count).linkTo(copy, &jit);
    isCopied.link(&jit);

    CCallHelpers::Label hasStorage = jit.label();
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfArrayAllocator()), allocatorOfCell);
    jit.emitAllocateWithNonNullAllocator(result, JITAllocator::variable(), allocatorOfCell, scratch, fails, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
    jit.load32(CCallHelpers::BaseIndex(instanceGPR, shape, CCallHelpers::TimesTwo, Instance::offsetOfOriginalArrayStructureIDs()), scratch);
    jit.store32(scratch, Address(result, JSCell::structureIDOffset()));
    jit.or32(TrustedImm32(TypeInfoBlob(ArrayClass, TypeInfo(ArrayType, JSArray::StructureFlags)).blob()), shape);
    jit.store32(shape, Address(result, JSCell::indexingTypeAndMiscOffset()));
    jit.addPtr(TrustedImm32(sizeof(IndexingHeader)), storage);
    jit.storePtr(storage, Address(result, JSObject::butterflyOffset()));
    fenceAfterAllocation(jit, scratch);
    returnToTypedCaller(jit, isLeaf);

    isOne.link(&jit);
    jit.loadPtr(Address(allocatorOfStorage, CompleteSubspace::offsetOfAllocatorForSizeStep() + MarkedSpace::sizeClassToIndex(sizeof(IndexingHeader) + sizeof(EncodedJSValue)) * sizeof(Allocator)), allocatorOfStorage);
    fails.append(jit.branchTestPtr(CCallHelpers::Zero, allocatorOfStorage));
    jit.emitAllocateWithNonNullAllocator(storage, JITAllocator::variable(), allocatorOfStorage, scratch, fails, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
    jit.store32(TrustedImm32(1), Address(storage, 0));
    jit.store32(TrustedImm32(1), Address(storage, sizeof(uint32_t)));
    jit.load64(Address(source, 0), scratch);
    jit.store64(scratch, Address(storage, sizeof(IndexingHeader)));
    jit.jump().linkTo(hasStorage, &jit);

    fails.link(&jit);
    jit.move(TrustedImm32(static_cast<unsigned>(isWhole ? Entry::operationAOTTypedArrayCopy : Entry::operationAOTTypedArraySlice) * sizeof(void*)), T9);
    generateColdOperation(jit, isLeaf, true);
}
static void generateArraySlice(CCallHelpers& jit) { generateArraySlice(jit, false); }
static void generateLeafArraySlice(CCallHelpers& jit) { jit.breakpoint(); }
static void generateArrayCopy(CCallHelpers& jit) { generateArraySlice(jit, true); }
static void generateLeafArrayCopy(CCallHelpers& jit) { jit.breakpoint(); }

static void generatePushToFullVector(CCallHelpers& jit, bool isLeaf)
{
    constexpr GPRReg result = GPRInfo::returnValueGPR;
    constexpr GPRReg allocator = T9;
#if CPU(ARM64)
    constexpr GPRReg scratch = T11;
    constexpr GPRReg storage = T12;
    constexpr GPRReg old = T13;
    constexpr GPRReg raw = T14;
#else
    constexpr GPRReg scratch = A0;
    constexpr GPRReg storage = A5;
    constexpr GPRReg old = A4;
    constexpr GPRReg raw = T13;
#endif
    static_assert(noOverlap(result, allocator, scratch, storage, old, raw, A1, A2, A3));
    static_assert(Butterfly::offsetOfPublicLength() == -static_cast<ptrdiff_t>(sizeof(IndexingHeader)) && Butterfly::offsetOfVectorLength() == Butterfly::offsetOfPublicLength() + static_cast<ptrdiff_t>(sizeof(uint32_t)));
    auto lengths = [](uint64_t publicLength, uint64_t vectorLength) { return CCallHelpers::TrustedImm64(vectorLength << 32 | publicLength); };

    CCallHelpers::JumpList slowCases;
    // There may be room: the site has the one kind of storage that it expects. A number goes where there is room for it, whichever way the array keeps its elements.
    {
        constexpr FPRReg number = FPRInfo::fpRegT0;
        jit.load8(Address(A1, JSCell::indexingTypeAndMiscOffset()), scratch);
        jit.and32(TrustedImm32(IndexingShapeAndWritabilityMask), scratch);
        jit.loadPtr(Address(A1, JSObject::butterflyOffset()), old);
        jit.load32(Address(old, Butterfly::offsetOfPublicLength()), result);
        Jump hasNoRoom = jit.branch32(CCallHelpers::AboveOrEqual, result, Address(old, Butterfly::offsetOfVectorLength()));
        Jump isNoNumber = jit.branchIfNotNumber(A2);
        Jump keepsDoubles = jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(DoubleShape));
        Jump keepsValues = jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(ContiguousShape));
        Jump keepsSomethingElse = jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(Int32Shape));
        Jump isNoInt32 = jit.branchIfNotInt32(A2);
        keepsValues.link(&jit);
        jit.store64(A2, CCallHelpers::BaseIndex(old, result, CCallHelpers::TimesEight));
        Jump isStored = jit.jump();
        keepsDoubles.link(&jit);
        // The caller's registers for floating point numbers are left as they are.
        jit.moveDoubleTo64(number, storage);
        Jump isInt32 = jit.branchIfInt32(A2);
        jit.add64(numberTag, A2, scratch);
        jit.move64ToDouble(scratch, number);
        // Among doubles it would read as a hole.
        Jump isNaN = jit.branchIfNaN(number);
        Jump isDouble = jit.jump();
        isInt32.link(&jit);
        jit.convertInt32ToDouble(A2, number);
        isDouble.link(&jit);
        jit.storeDouble(number, CCallHelpers::BaseIndex(old, result, CCallHelpers::TimesEight));
        jit.move64ToDouble(storage, number);
        isStored.link(&jit);
        jit.add32(TrustedImm32(1), result);
        jit.store32(result, Address(old, Butterfly::offsetOfPublicLength()));
        jit.or64(numberTag, result);
        returnToTypedCaller(jit, isLeaf);
        // The site stores it itself, unless the array has to be told.
        isNoNumber.link(&jit);
        Jump doesNotKeepValues = jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(ContiguousShape));
        jit.store64(A2, CCallHelpers::BaseIndex(old, result, CCallHelpers::TimesEight));
        jit.add32(TrustedImm32(1), result);
        jit.store32(result, Address(old, Butterfly::offsetOfPublicLength()));
        jit.or64(numberTag, result);
        static_assert(noOverlap(T9, T11, A1, A2, result));
        static_assert(noOverlap(scratch, storage, A1, A2, result));
        CCallHelpers::JumpList needNotBeTold;
        needNotBeTold.append(jit.branchIfNotCell(A2));
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfVM()), scratch);
        jit.load8(Address(A1, JSCell::cellStateOffset()), storage);
        needNotBeTold.append(jit.branch32(CCallHelpers::Above, storage, Address(scratch, VM::offsetOfHeapBarrierThreshold())));
        jit.move(A1, T9);
        jit.move(A2, T11);
        jit.jump().linkTo(stubLabels()[static_cast<unsigned>(isLeaf ? Stub::LeafRememberElement : Stub::RememberElement)], &jit);
        needNotBeTold.link(&jit);
        returnToTypedCaller(jit, isLeaf);
        isNaN.link(&jit);
        jit.move64ToDouble(storage, number);
        hasNoRoom.link(&jit);
        doesNotKeepValues.link(&jit);
        keepsSomethingElse.link(&jit);
        isNoInt32.link(&jit);
    }
    slowCases.append(jit.branch64(CCallHelpers::Above, A3, TrustedImm32(1)));
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfVM()), scratch);
    slowCases.append(jit.branchTest8(CCallHelpers::NonZero, Address(scratch, VM::offsetOfHeapMutatorShouldBeFenced())));
    jit.load8(Address(A1, JSCell::cellStateOffset()), result);
    slowCases.append(jit.branch32(CCallHelpers::BelowOrEqual, result, Address(scratch, VM::offsetOfHeapBarrierThreshold())));
    jit.load32(Address(A1, JSCell::structureIDOffset()), scratch);
    jit.loadPtr(Address(A1, JSObject::butterflyOffset()), old);
    Jump doesNotKeepValues = jit.branch32(CCallHelpers::NotEqual, scratch, Address(instanceGPR, Instance::offsetOfNewArrayWithContiguousStructureID()));
    jit.load64(Address(old, Butterfly::offsetOfPublicLength()), result);
    auto grow = [&](unsigned from, unsigned to) {
        size_t size = sizeof(IndexingHeader) + to * sizeof(EncodedJSValue);
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfAuxiliarySpace()), allocator);
        jit.loadPtr(Address(allocator, CompleteSubspace::offsetOfAllocatorForSizeStep() + MarkedSpace::sizeClassToIndex(size) * sizeof(Allocator)), allocator);
        slowCases.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
        jit.emitAllocateWithNonNullAllocator(storage, JITAllocator::variable(), allocator, scratch, slowCases, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
        jit.store64(lengths(from + 1, to), Address(storage, 0));
        for (unsigned i = 0; i < from; ++i) {
            jit.load64(Address(old, i * sizeof(EncodedJSValue)), scratch);
            jit.store64(scratch, Address(storage, sizeof(IndexingHeader) + i * sizeof(EncodedJSValue)));
        }
        jit.store64(A2, Address(storage, sizeof(IndexingHeader) + from * sizeof(EncodedJSValue)));
        for (unsigned i = from + 1; i < to; ++i)
            jit.store64(TrustedImm32(0), Address(storage, sizeof(IndexingHeader) + i * sizeof(EncodedJSValue)));
        jit.addPtr(TrustedImm32(sizeof(IndexingHeader)), storage);
        jit.storePtr(storage, Address(A1, JSObject::butterflyOffset()));
        jit.move(CCallHelpers::TrustedImm64(JSValue::encode(jsNumber(from + 1))), result);
        returnToTypedCaller(jit, isLeaf);
    };
    Jump hasOne = jit.branch64(CCallHelpers::Equal, result, lengths(1, 1));
    Jump hasAnotherLength = jit.branch64(CCallHelpers::NotEqual, result, lengths(3, 3));
    grow(3, 7);
    hasOne.link(&jit);
    grow(1, 3);

    auto originalStructureID = [](IndexingType shape) {
        return Address(instanceGPR, Instance::offsetOfOriginalArrayStructureIDs() + (shape >> Instance::arrayKindShift) * sizeof(uint32_t));
    };
    constexpr FPRReg number = FPRInfo::fpRegT0;
    constexpr GPRReg index = allocator;
    CCallHelpers::JumpList isTaken;
    hasAnotherLength.link(&jit);
    jit.move(A2, raw);
    isTaken.append(jit.jump());
    doesNotKeepValues.link(&jit);
    Jump keepsDoubles = jit.branch32(CCallHelpers::Equal, scratch, originalStructureID(DoubleShape));
    slowCases.append(jit.branch32(CCallHelpers::NotEqual, scratch, originalStructureID(Int32Shape)));
    slowCases.append(jit.branchIfNotInt32(A2));
    jit.move(A2, raw);
    isTaken.append(jit.jump());
    keepsDoubles.link(&jit);
    slowCases.append(jit.branchIfNotNumber(A2));
    Jump isInt32 = jit.branchIfInt32(A2);
    jit.add64(numberTag, A2, raw);
    jit.lshift64(raw, TrustedImm32(1), scratch);
    slowCases.append(jit.branch64(CCallHelpers::Above, scratch, CCallHelpers::TrustedImm64(static_cast<int64_t>(0xffe0000000000000ull))));
    isTaken.append(jit.jump());
    isInt32.link(&jit);
    jit.moveDoubleTo64(number, scratch);
    jit.convertInt32ToDouble(A2, number);
    jit.moveDoubleTo64(number, raw);
    jit.move64ToDouble(scratch, number);

    isTaken.link(&jit);
    jit.load32(Address(old, Butterfly::offsetOfPublicLength()), result);
    slowCases.append(jit.branch32(CCallHelpers::NotEqual, result, Address(old, Butterfly::offsetOfVectorLength())));
    slowCases.append(jit.branch32(CCallHelpers::Above, result, TrustedImm32(Instance::maxVectorLengthGrownByStub)));
    jit.load8(CCallHelpers::BaseIndex(instanceGPR, result, CCallHelpers::TimesOne, Instance::offsetOfNextVectorLengths()), scratch);
    slowCases.append(jit.branchTest32(CCallHelpers::Zero, scratch));
    static_assert(sizeof(IndexingHeader) == sizeof(EncodedJSValue) && MarkedSpace::sizeStep == 2 * sizeof(EncodedJSValue) && sizeof(Allocator) == sizeof(void*));
    jit.add32(TrustedImm32(1), scratch);
    jit.urshift32(TrustedImm32(1), scratch);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfAuxiliarySpace()), allocator);
    jit.loadPtr(CCallHelpers::BaseIndex(allocator, scratch, CCallHelpers::TimesEight, CompleteSubspace::offsetOfAllocatorForSizeStep()), allocator);
    slowCases.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    jit.emitAllocateWithNonNullAllocator(storage, JITAllocator::variable(), allocator, scratch, slowCases, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
    jit.move(TrustedImm32(0), index);
    CCallHelpers::Label copy = jit.label();
    jit.load64(CCallHelpers::BaseIndex(old, index, CCallHelpers::TimesEight), scratch);
    jit.store64(scratch, CCallHelpers::BaseIndex(storage, index, CCallHelpers::TimesEight, sizeof(IndexingHeader)));
    jit.add32(TrustedImm32(1), index);
    jit.branch32(CCallHelpers::Below, index, result).linkTo(copy, &jit);
    jit.store64(raw, CCallHelpers::BaseIndex(storage, index, CCallHelpers::TimesEight, sizeof(IndexingHeader)));
    jit.add32(TrustedImm32(1), index);
    jit.load8(CCallHelpers::BaseIndex(instanceGPR, result, CCallHelpers::TimesOne, Instance::offsetOfNextVectorLengths()), scratch);
    jit.store32(index, Address(storage, 0));
    jit.store32(scratch, Address(storage, sizeof(uint32_t)));
    jit.load8(Address(A1, JSCell::indexingTypeAndMiscOffset()), raw);
    jit.and32(TrustedImm32(IndexingShapeMask), raw);
    Jump clearsDoubles = jit.branch32(CCallHelpers::Equal, raw, TrustedImm32(DoubleShape));
    jit.move(TrustedImm32(0), raw);
    Jump hasHole = jit.jump();
    clearsDoubles.link(&jit);
    jit.move(CCallHelpers::TrustedImm64(std::bit_cast<int64_t>(PNaN)), raw);
    hasHole.link(&jit);
    Jump isCleared = jit.branch32(CCallHelpers::AboveOrEqual, index, scratch);
    CCallHelpers::Label clear = jit.label();
    jit.store64(raw, CCallHelpers::BaseIndex(storage, index, CCallHelpers::TimesEight, sizeof(IndexingHeader)));
    jit.add32(TrustedImm32(1), index);
    jit.branch32(CCallHelpers::Below, index, scratch).linkTo(clear, &jit);
    isCleared.link(&jit);
    jit.addPtr(TrustedImm32(sizeof(IndexingHeader)), storage);
    jit.storePtr(storage, Address(A1, JSObject::butterflyOffset()));
    jit.add32(TrustedImm32(1), result);
    jit.or64(numberTag, result);
    returnToTypedCaller(jit, isLeaf);

    slowCases.link(&jit);
    jit.move(TrustedImm32(static_cast<unsigned>(Entry::operationAOTTypedArrayPush) * sizeof(void*)), T9);
    generateColdOperation(jit, isLeaf, true);
}
static void generatePushToFullVector(CCallHelpers& jit) { generatePushToFullVector(jit, false); }
#if CPU(ARM64)
static void generateLeafPushToFullVector(CCallHelpers& jit) { generatePushToFullVector(jit, true); }
#else
static void generateLeafPushToFullVector(CCallHelpers& jit) { jit.breakpoint(); }
#endif

static void generateArrayPushOrCall(CCallHelpers& jit, Vector<std::pair<Jump, unsigned>>& isLeftToCall)
{
    constexpr GPRReg expected = T11;
    static_assert(noOverlap(expected, thisGPR, calleeGPR, argumentGPR(0)) && noOverlap(A2, thisGPR) && noOverlap(A1, A2, A3));
    isLeftToCall.append({ jit.branchIfNotCell(thisGPR), 1 });
    jit.load8(Address(thisGPR, JSCell::indexingTypeAndMiscOffset()), expected);
    jit.urshift32(TrustedImm32(Instance::arrayKindShift), expected);
    jit.and32(TrustedImm32(Instance::numberOfArrayKinds - 1), expected);
    jit.load32(CCallHelpers::BaseIndex(instanceGPR, expected, CCallHelpers::TimesFour, Instance::offsetOfOriginalArrayStructureIDs()), expected);
    isLeftToCall.append({ jit.branch32(CCallHelpers::NotEqual, expected, Address(thisGPR, JSCell::structureIDOffset())), 1 });
    jit.move(argumentGPR(0), A2);
    jit.move(thisGPR, A1);
    jit.move(TrustedImm32(0), A3);
    jit.jump().linkTo(stubLabels()[static_cast<unsigned>(Stub::PushToFullVector)], &jit);
}
static void generateArrayPushOrCallMethod(CCallHelpers& jit) { generateArrayPushOrCall(jit, jumpsFromStubsToCallMethod()); }
static void generateArrayPushOrCallValue(CCallHelpers& jit) { generateArrayPushOrCall(jit, jumpsFromStubsToCallValue()); }

static void generateMethodOfTableOrCall(CCallHelpers& jit, Receiver receiver, unsigned count, Stub stub, Vector<std::pair<Jump, unsigned>>& isLeftToCall)
{
    constexpr GPRReg structureID = T11;
    static_assert(noOverlap(structureID, thisGPR, calleeGPR, argumentGPR(0), argumentGPR(1)));
    isLeftToCall.append({ jit.branchIfNotCell(thisGPR), count });
    jit.load32(Address(thisGPR, JSCell::structureIDOffset()), structureID);
    isLeftToCall.append({ jit.branch32(CCallHelpers::NotEqual, structureID, Address(instanceGPR, Instance::offsetOfReceiverStructureIDs() + static_cast<unsigned>(receiver) * sizeof(uint32_t))), count });
    jit.jump().linkTo(stubLabels()[static_cast<unsigned>(stub)], &jit);
}
static void generateMapGetOrCallMethod(CCallHelpers& jit) { generateMethodOfTableOrCall(jit, Receiver::Map, 1, Stub::MapGet, jumpsFromStubsToCallMethod()); }
static void generateMapGetOrCallValue(CCallHelpers& jit) { generateMethodOfTableOrCall(jit, Receiver::Map, 1, Stub::MapGet, jumpsFromStubsToCallValue()); }
static void generateMapHasOrCallMethod(CCallHelpers& jit) { generateMethodOfTableOrCall(jit, Receiver::Map, 1, Stub::MapHas, jumpsFromStubsToCallMethod()); }
static void generateMapHasOrCallValue(CCallHelpers& jit) { generateMethodOfTableOrCall(jit, Receiver::Map, 1, Stub::MapHas, jumpsFromStubsToCallValue()); }
static void generateMapSetOrCallMethod(CCallHelpers& jit) { generateMethodOfTableOrCall(jit, Receiver::Map, 2, Stub::MapSet, jumpsFromStubsToCallMethod()); }
static void generateMapSetOrCallValue(CCallHelpers& jit) { generateMethodOfTableOrCall(jit, Receiver::Map, 2, Stub::MapSet, jumpsFromStubsToCallValue()); }
static void generateSetHasOrCallMethod(CCallHelpers& jit) { generateMethodOfTableOrCall(jit, Receiver::Set, 1, Stub::SetHas, jumpsFromStubsToCallMethod()); }
static void generateSetHasOrCallValue(CCallHelpers& jit) { generateMethodOfTableOrCall(jit, Receiver::Set, 1, Stub::SetHas, jumpsFromStubsToCallValue()); }
static void generateSetAddOrCallMethod(CCallHelpers& jit) { generateMethodOfTableOrCall(jit, Receiver::Set, 1, Stub::SetAdd, jumpsFromStubsToCallMethod()); }
static void generateSetAddOrCallValue(CCallHelpers& jit) { generateMethodOfTableOrCall(jit, Receiver::Set, 1, Stub::SetAdd, jumpsFromStubsToCallValue()); }

static void generatePartOfString(CCallHelpers& jit, bool isLeaf)
{
    constexpr GPRReg result = GPRInfo::returnValueGPR;
    constexpr GPRReg span = T9;
#if CPU(ARM64)
    constexpr GPRReg fiber = T11;
    constexpr GPRReg flags = T12;
    constexpr GPRReg base = T13;
    constexpr GPRReg start = T14;
#else
    constexpr GPRReg fiber = T10;
    constexpr GPRReg flags = T11;
    constexpr GPRReg base = T12;
    constexpr GPRReg start = T13;
#endif
    constexpr GPRReg allocator = fiber;
    constexpr GPRReg scratch = span;
    static_assert(noOverlap(result, span, fiber, flags, base, start, A1, A2, A3));
    static_assert(StringImpl::flagIs8Bit() == JSRopeString::is8BitInPointer);
    static_assert(JSRopeString::offsetOfFiber0() == sizeof(EncodedJSValue) && JSRopeString::offsetOfLength() == 2 * sizeof(EncodedJSValue));
    static_assert(maxSingleCharacterString == std::numeric_limits<Latin1Character>::max());

    CCallHelpers::JumpList fails;
    CCallHelpers::JumpList isWhole;
    jit.sub64(A3, A2, span);
    Jump isAtMostOne = jit.branch64(CCallHelpers::LessThanOrEqual, span, TrustedImm32(1));
    jit.loadPtr(Address(A1, JSString::offsetOfValue()), fiber);
    Jump isRope = jit.branchIfRopeStringImpl(fiber);
    isWhole.append(jit.branch32(CCallHelpers::Equal, span, Address(fiber, StringImpl::lengthMemoryOffset())));
    jit.load32(Address(fiber, StringImpl::flagsOffset()), flags);
    jit.move(A1, base);
    jit.move(A2, start);
    Jump isReady = jit.jump();

    isRope.link(&jit);
    isWhole.append(jit.branch32(CCallHelpers::Equal, span, Address(A1, JSRopeString::offsetOfLength())));
    fails.append(jit.branchTestPtr(CCallHelpers::Zero, fiber, TrustedImm32(JSRopeString::isSubstringInPointer)));
    jit.move(fiber, flags);
    jit.load64(Address(A1, 2 * sizeof(EncodedJSValue)), base);
    jit.load64(Address(A1, 3 * sizeof(EncodedJSValue)), start);
    jit.urshift64(TrustedImm32(32), base);
    jit.move(start, scratch);
    jit.and64(TrustedImm32(0xffff), scratch);
    jit.lshift64(TrustedImm32(32), scratch);
    jit.or64(scratch, base);
    jit.urshift64(TrustedImm32(16), start);
    jit.add64(A2, start);

    isReady.link(&jit);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfRopeStringAllocator()), allocator);
    fails.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    jit.emitAllocateWithNonNullAllocator(result, JITAllocator::variable(), allocator, scratch, fails, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
    jit.load64(Address(instanceGPR, Instance::offsetOfStringHeader()), scratch);
    jit.and32(TrustedImm32(JSRopeString::is8BitInPointer), flags);
    jit.or64(TrustedImm32(JSString::isRopeInPointer | JSRopeString::isSubstringInPointer), flags);
    jit.storePair64(scratch, flags, result, TrustedImm32(0));
    jit.sub64(A3, A2, span);
    jit.move(base, flags);
    jit.lshift64(TrustedImm32(32), flags);
    jit.or64(span, flags);
    jit.urshift64(TrustedImm32(32), base);
    jit.lshift64(TrustedImm32(16), start);
    jit.or64(start, base);
    jit.storePair64(flags, base, result, TrustedImm32(2 * sizeof(EncodedJSValue)));
    fenceAfterAllocation(jit, scratch);
    returnToTypedCaller(jit, isLeaf);

    isWhole.link(&jit);
    jit.move(A1, result);
    returnToTypedCaller(jit, isLeaf);

    isAtMostOne.link(&jit);
    Jump isOne = jit.branch64(CCallHelpers::Equal, span, TrustedImm32(1));
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfEmptyString()), result);
    returnToTypedCaller(jit, isLeaf);

    isOne.link(&jit);
    jit.loadPtr(Address(A1, JSString::offsetOfValue()), fiber);
    fails.append(jit.branchIfRopeStringImpl(fiber));
    jit.load32(Address(fiber, StringImpl::flagsOffset()), flags);
    jit.loadPtr(Address(fiber, StringImpl::dataOffset()), fiber);
    Jump isWide = jit.branchTest32(CCallHelpers::Zero, flags, TrustedImm32(StringImpl::flagIs8Bit()));
    jit.load8(CCallHelpers::BaseIndex(fiber, A2, CCallHelpers::TimesOne), flags);
    Jump hasCodeUnit = jit.jump();
    isWide.link(&jit);
    jit.load16(CCallHelpers::BaseIndex(fiber, A2, CCallHelpers::TimesTwo), flags);
    fails.append(jit.branch32(CCallHelpers::Above, flags, TrustedImm32(maxSingleCharacterString)));
    hasCodeUnit.link(&jit);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfSingleCharacterStrings()), fiber);
    jit.loadPtr(CCallHelpers::BaseIndex(fiber, flags, CCallHelpers::TimesEight), result);
    returnToTypedCaller(jit, isLeaf);

    fails.link(&jit);
    jit.move(TrustedImm32(static_cast<unsigned>(Entry::operationAOTTypedSubstring) * sizeof(void*)), T9);
    generateColdOperation(jit, isLeaf, true);
}
static void generatePartOfString(CCallHelpers& jit) { generatePartOfString(jit, false); }
#if CPU(ARM64)
static void generateLeafPartOfString(CCallHelpers& jit) { generatePartOfString(jit, true); }
#else
static void generateLeafPartOfString(CCallHelpers& jit) { jit.breakpoint(); }
#endif

static void generatePushDoubleToFullVector(CCallHelpers& jit, bool isLeaf)
{
    constexpr FPRReg number = FPRInfo::fpRegT0;
    constexpr FPRReg other = FPRInfo::fpRegT1;
#if CPU(ARM64)
    constexpr GPRReg keptNumber = T11;
    constexpr GPRReg integer = T13;
#else
    constexpr GPRReg keptNumber = A0;
    constexpr GPRReg integer = A4;
#endif
    static_assert(noOverlap(keptNumber, integer, A1, A2, A3));
    jit.moveDoubleTo64(number, keptNumber);
    jit.move64ToDouble(A2, number);
    CCallHelpers::JumpList isNoInt32;
    branchIfNoExactInt32(jit, number, integer, isNoInt32, other);
    jit.or64(numberTag, integer, A2);
    Jump isValue = jit.jump();
    isNoInt32.link(&jit);
    jit.purifyNaN(number, number);
    jit.moveDoubleTo64(number, A2);
    jit.sub64(numberTag, A2);
    isValue.link(&jit);
    jit.move64ToDouble(keptNumber, number);
    jit.jump().linkTo(stubLabels()[static_cast<unsigned>(isLeaf ? Stub::LeafPushToFullVector : Stub::PushToFullVector)], &jit);
}
static void generatePushDoubleToFullVector(CCallHelpers& jit) { generatePushDoubleToFullVector(jit, false); }
#if CPU(ARM64)
static void generateLeafPushDoubleToFullVector(CCallHelpers& jit) { generatePushDoubleToFullVector(jit, true); }
#else
static void generateLeafPushDoubleToFullVector(CCallHelpers& jit) { jit.breakpoint(); }
#endif

static void generateStoreElement(CCallHelpers& jit, bool isLeaf)
{
    constexpr GPRReg mode = T9;
    constexpr GPRReg butterfly = T11;
    constexpr GPRReg length = T12;
    constexpr GPRReg savedNumber = T13;
    constexpr FPRReg number = FPRInfo::fpRegT0;
    static_assert(noOverlap(mode, T10, butterfly, length, savedNumber, A1, A2, A3));
    CCallHelpers::JumpList generally;
    auto makeRoom = [&](CCallHelpers::JumpList& hasNoRoom) {
        jit.load32(Address(butterfly, Butterfly::offsetOfPublicLength()), length);
        Jump isWithinLength = jit.branch64(CCallHelpers::Below, A2, length);
        jit.load32(Address(butterfly, Butterfly::offsetOfVectorLength()), length);
        hasNoRoom.append(jit.branch64(CCallHelpers::AboveOrEqual, A2, length));
        jit.add32(TrustedImm32(1), A2, length);
        jit.store32(length, Address(butterfly, Butterfly::offsetOfPublicLength()));
        isWithinLength.link(&jit);
    };
    jit.load8(Address(A1, JSCell::indexingTypeAndMiscOffset()), mode);
    jit.and32(TrustedImm32(IndexingShapeAndWritabilityMask), mode);
    jit.loadPtr(Address(A1, JSObject::butterflyOffset()), butterfly);
    Jump keepsValues = jit.branch32(CCallHelpers::Equal, mode, TrustedImm32(ContiguousShape));
    Jump keepsDoubles = jit.branch32(CCallHelpers::Equal, mode, TrustedImm32(DoubleShape));
    generally.append(jit.branch32(CCallHelpers::NotEqual, mode, TrustedImm32(Int32Shape)));
    generally.append(jit.branch64(CCallHelpers::Below, A3, numberTag));
    makeRoom(generally);
    jit.store64(A3, CCallHelpers::BaseIndex(butterfly, A2, CCallHelpers::TimesEight));
    returnToTypedCaller(jit, isLeaf);

    keepsValues.link(&jit);
    makeRoom(generally);
    jit.store64(A3, CCallHelpers::BaseIndex(butterfly, A2, CCallHelpers::TimesEight));
    Jump isNoCell = jit.branchIfNotCell(A3);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfVM()), length);
    jit.load8(Address(A1, JSCell::cellStateOffset()), mode);
    Jump isBeyondThreshold = jit.branch32(CCallHelpers::Above, mode, Address(length, VM::offsetOfHeapBarrierThreshold()));
    static_assert(noOverlap(T9, T11, A1, A3));
    jit.move(A1, T9);
    jit.move(A3, T11);
    jit.jump().linkTo(stubLabels()[static_cast<unsigned>(isLeaf ? Stub::LeafRememberElement : Stub::RememberElement)], &jit);
    isNoCell.link(&jit);
    isBeyondThreshold.link(&jit);
    returnToTypedCaller(jit, isLeaf);

    keepsDoubles.link(&jit);
    generally.append(jit.branchTest64(CCallHelpers::Zero, A3, numberTag));
    CCallHelpers::JumpList generallyWithNumberPutBack;
    jit.moveDoubleTo64(number, savedNumber);
    Jump isInt32 = jit.branch64(CCallHelpers::AboveOrEqual, A3, numberTag);
    jit.add64(numberTag, A3, length);
    jit.move64ToDouble(length, number);
    generallyWithNumberPutBack.append(jit.branchIfNaN(number));
    Jump isUnboxed = jit.jump();
    isInt32.link(&jit);
    jit.convertInt32ToDouble(A3, number);
    isUnboxed.link(&jit);
    makeRoom(generallyWithNumberPutBack);
    jit.storeDouble(number, CCallHelpers::BaseIndex(butterfly, A2, CCallHelpers::TimesEight));
    jit.move64ToDouble(savedNumber, number);
    returnToTypedCaller(jit, isLeaf);

    generallyWithNumberPutBack.link(&jit);
    jit.move64ToDouble(savedNumber, number);
    generally.link(&jit);
    jit.move(TrustedImm32(static_cast<unsigned>(Entry::operationAOTTypedStoreElement) * sizeof(void*)), T9);
    generateColdOperation(jit, isLeaf);
}
static void generateStoreElement(CCallHelpers& jit) { generateStoreElement(jit, false); }
static void generateLoadElement(CCallHelpers& jit, bool isLeaf)
{
    constexpr GPRReg mode = T9;
    constexpr GPRReg butterfly = T11;
    constexpr GPRReg length = T12;
    constexpr GPRReg result = GPRInfo::returnValueGPR;
    constexpr GPRReg savedNumber = T13;
    constexpr GPRReg savedScratch = T14;
    constexpr FPRReg number = FPRInfo::fpRegT0;
    constexpr FPRReg scratch = FPRInfo::fpRegT1;
    static_assert(noOverlap(mode, T10, butterfly, length, savedNumber, savedScratch, result, A1, A2));
    CCallHelpers::JumpList generally;
    CCallHelpers::JumpList absent;
    jit.load8(Address(A1, JSCell::indexingTypeAndMiscOffset()), mode);
    jit.and32(TrustedImm32(IndexingShapeMask), mode);
    jit.loadPtr(Address(A1, JSObject::butterflyOffset()), butterfly);
    jit.load32(Address(butterfly, Butterfly::offsetOfPublicLength()), length);
    Jump keepsValues = jit.branch32(CCallHelpers::Equal, mode, TrustedImm32(ContiguousShape));
    Jump keepsDoubles = jit.branch32(CCallHelpers::Equal, mode, TrustedImm32(DoubleShape));
    generally.append(jit.branch32(CCallHelpers::NotEqual, mode, TrustedImm32(Int32Shape)));
    keepsValues.link(&jit);
    absent.append(jit.branch64(CCallHelpers::AboveOrEqual, A2, length));
    jit.load64(CCallHelpers::BaseIndex(butterfly, A2, CCallHelpers::TimesEight), result);
    absent.append(jit.branchTest64(CCallHelpers::Zero, result));
    returnToTypedCaller(jit, isLeaf);

    keepsDoubles.link(&jit);
    absent.append(jit.branch64(CCallHelpers::AboveOrEqual, A2, length));
    jit.moveDoubleTo64(number, savedNumber);
    jit.moveDoubleTo64(scratch, savedScratch);
    jit.loadDouble(CCallHelpers::BaseIndex(butterfly, A2, CCallHelpers::TimesEight), number);
    Jump isHole = jit.branchIfNaN(number);
    CCallHelpers::JumpList isNoInt32;
    branchIfNoExactInt32(jit, number, length, isNoInt32, scratch);
    jit.add64(numberTag, length, result);
    Jump isBoxed = jit.jump();
    isNoInt32.link(&jit);
    jit.moveDoubleTo64(number, result);
    jit.sub64(numberTag, result);
    isBoxed.link(&jit);
    jit.move64ToDouble(savedNumber, number);
    jit.move64ToDouble(savedScratch, scratch);
    returnToTypedCaller(jit, isLeaf);

    isHole.link(&jit);
    jit.move64ToDouble(savedNumber, number);
    jit.move64ToDouble(savedScratch, scratch);
    absent.link(&jit);
    generally.append(jit.branch8(CCallHelpers::NotEqual, Address(A1, JSCell::typeInfoTypeOffset()), TrustedImm32(ArrayType)));
    generally.append(jit.branch64(CCallHelpers::Above, A2, CCallHelpers::TrustedImm64(MAX_ARRAY_INDEX)));
    jit.move(CCallHelpers::TrustedImm64(JSValue::encode(jsUndefined())), result);
    returnToTypedCaller(jit, isLeaf);

    generally.link(&jit);
    jit.move(TrustedImm32(static_cast<unsigned>(Entry::operationAOTTypedArrayGetAtIndex) * sizeof(void*)), T9);
    generateColdOperation(jit, isLeaf, true);
}
static void generateLoadElement(CCallHelpers& jit) { generateLoadElement(jit, false); }
#if CPU(ARM64)
static void generateLeafLoadElement(CCallHelpers& jit) { generateLoadElement(jit, true); }
#else
static void generateLeafLoadElement(CCallHelpers& jit) { jit.breakpoint(); }
#endif
#if CPU(ARM64)
static void generateLeafStoreElement(CCallHelpers& jit) { generateStoreElement(jit, true); }
#else
static void generateLeafStoreElement(CCallHelpers& jit) { jit.breakpoint(); }
#endif

#define AOT_GENERATE_NEW_OBJECT_LITERAL(n) static void generateNewObjectLiteral##n(CCallHelpers& jit) { generateNewObjectLiteral(jit, n); }
AOT_GENERATE_NEW_OBJECT_LITERAL(0)
AOT_GENERATE_NEW_OBJECT_LITERAL(1)
AOT_GENERATE_NEW_OBJECT_LITERAL(2)
AOT_GENERATE_NEW_OBJECT_LITERAL(3)
AOT_GENERATE_NEW_OBJECT_LITERAL(4)
AOT_GENERATE_NEW_OBJECT_LITERAL(5)
AOT_GENERATE_NEW_OBJECT_LITERAL(6)
#undef AOT_GENERATE_NEW_OBJECT_LITERAL
#define AOT_GENERATE_NEW_OBJECT_OF_ROW(n) static void generateNewObjectOfRow##n(CCallHelpers& jit) { generateNewObjectLiteral(jit, n, true); }
AOT_GENERATE_NEW_OBJECT_OF_ROW(0)
AOT_GENERATE_NEW_OBJECT_OF_ROW(1)
AOT_GENERATE_NEW_OBJECT_OF_ROW(2)
AOT_GENERATE_NEW_OBJECT_OF_ROW(3)
AOT_GENERATE_NEW_OBJECT_OF_ROW(4)
AOT_GENERATE_NEW_OBJECT_OF_ROW(5)
AOT_GENERATE_NEW_OBJECT_OF_ROW(6)
#undef AOT_GENERATE_NEW_OBJECT_OF_ROW

// A1: the site's slots, which are for another new.target or for none. A2: the constructor. A3: new.target. See Instance::thisAllocations.
static void generateCreateThisByNewTarget(CCallHelpers& jit)
{
    constexpr GPRReg entry = T9;
    constexpr GPRReg epoch = T10;
    static_assert(noOverlap(entry, epoch, T11, A0, A1, A2, A3));
    CCallHelpers::JumpList miss;
    // Only for a site that cannot learn: it has given up, or its slots are nobody's, which reads the same. One that can goes on to the operation, which fills it.
    jit.load32(Address(A1, OBJECT_OFFSETOF(Slot, offset)), epoch);
    jit.and32(TrustedImm32(Slot::attemptsMask), epoch);
    miss.append(jit.branch32(CCallHelpers::NotEqual, epoch, TrustedImm32(Slot::attemptsMask)));
    jit.urshift64(A3, TrustedImm32(4), entry);
    jit.and32(TrustedImm32(Instance::numberOfThisAllocations - 1), entry);
    jit.lshift64(TrustedImm32(WTF::fastLog2(static_cast<unsigned>(Instance::slotsPerThisAllocation * sizeof(Slot)))), entry);
    jit.addPtr(instanceGPR, entry);
    jit.addPtr(TrustedImm32(Instance::offsetOfThisAllocations()), entry);
    miss.append(jit.branchTest32(CCallHelpers::Zero, Address(entry, OBJECT_OFFSETOF(Slot, structureID))));
    miss.append(jit.branchPtr(CCallHelpers::NotEqual, Address(entry, OBJECT_OFFSETOF(Slot, pointer)), A3));
    miss.append(jit.branchPtr(CCallHelpers::NotEqual, Address(entry, 3 * sizeof(Slot) + OBJECT_OFFSETOF(Slot, pointer)), A2));
    jit.load32(Address(entry, 2 * sizeof(Slot) + OBJECT_OFFSETOF(Slot, pointer)), epoch);
    miss.append(jit.branch32(CCallHelpers::NotEqual, epoch, Address(instanceGPR, Instance::offsetOfTypedPrototypeEpoch())));
    jit.move(entry, A1);
    jit.jump().linkTo(stubLabels()[static_cast<unsigned>(Stub::NewObjectLiteral0)], &jit);

    miss.link(&jit);
    jit.move(instanceGPR, A0);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T11);
    jit.loadPtr(Address(T11, static_cast<unsigned>(Entry::operationAOTTypedCreateThisAt) * sizeof(void*)), T11);
    callAndCheckException(jit, T11, Returns::Value);
}

#define AOT_GENERATE_HELPER(name) static void generate##name(CCallHelpers& jit) { generateHelper(jit, Stub::name); }
FOR_EACH_AOT_HELPER(AOT_GENERATE_HELPER)
#undef AOT_GENERATE_HELPER

#define AOT_GENERATE_STUB_NAMED(name) \
    case Stub::name: \
        generate##name(jit); \
        return;
static void generateStub(CCallHelpers& jit, Stub stub)
{
    switch (stub) {
    FOR_EACH_AOT_STUB(AOT_GENERATE_STUB_NAMED)
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}
#undef AOT_GENERATE_STUB_NAMED


#else // CPU(ARM64) || CPU(X86_64)

static void generateStub(CCallHelpers& jit, Stub) { jit.breakpoint(); }

#endif // CPU(ARM64) || CPU(X86_64)

static constexpr Stub operationCallStubs[] = {
    Stub::OperationValue, Stub::OperationVoid, Stub::OperationDouble, Stub::OperationValueWithGlobalObject,
    Stub::OperationValueWithInstance, Stub::OperationVoidWithInstance, Stub::OperationDoubleWithInstance,
    Stub::PlainOperation, Stub::PlainOperationWithInstance,
    Stub::ColdOperationVoid, Stub::LeafColdOperationVoid, Stub::ColdOperationValue, Stub::LeafColdOperationValue,
    Stub::ColdPlainOperation, Stub::LeafColdPlainOperation, Stub::ColdPlainOperationWithoutInstance, Stub::LeafColdPlainOperationWithoutInstance,
    Stub::ColdPlainOperationDouble, Stub::LeafColdPlainOperationDouble, Stub::ColdPlainOperationDoubleWithoutInstance, Stub::LeafColdPlainOperationDoubleWithoutInstance,
    Stub::ColdOperationDouble, Stub::LeafColdOperationDouble,
    Stub::ColdOperationVoidWithoutFPRs, Stub::LeafColdOperationVoidWithoutFPRs, Stub::ColdOperationValueWithoutFPRs, Stub::LeafColdOperationValueWithoutFPRs,
    Stub::ColdPlainOperationWithoutFPRs, Stub::LeafColdPlainOperationWithoutFPRs,
};
static constexpr Stub functionCallStubs[] = { Stub::Call, Stub::CallHostFunction, Stub::CallInternalFunction, Stub::ConstructInternalFunction, Stub::CallValue, Stub::CallValueWithoutThis, Stub::ConstructClosure, Stub::CallMethod, Stub::CallValueNamed };
static constexpr unsigned numberOfCountsWithThunk = numberOfArgumentGPRs + 1;
static constexpr unsigned firstCallThunk = std::size(operationCallStubs) * numberOfEntries;
static constexpr unsigned firstCallThunkOf(Stub stub)
{
    unsigned index = 0;
    while (functionCallStubs[index] != stub)
        ++index;
    return firstCallThunk + index * numberOfCountsWithThunk;
}
static constexpr unsigned numberOfThunks = firstCallThunk + std::size(functionCallStubs) * numberOfCountsWithThunk;
static_assert(numberOfThunks < std::numeric_limits<uint16_t>::max());

static bool callsOperation(Stub stub)
{
    for (Stub other : operationCallStubs) {
        if (other == stub)
            return true;
    }
    return false;
}

static constexpr bool isColdOperationWithThunk(Stub stub)
{
    switch (stub) {
    case Stub::ColdOperationVoid:
    case Stub::LeafColdOperationVoid:
    case Stub::ColdOperationValue:
    case Stub::LeafColdOperationValue:
    case Stub::ColdPlainOperation:
    case Stub::LeafColdPlainOperation:
    case Stub::ColdPlainOperationWithoutInstance:
    case Stub::LeafColdPlainOperationWithoutInstance:
    case Stub::ColdPlainOperationDouble:
    case Stub::LeafColdPlainOperationDouble:
    case Stub::ColdPlainOperationDoubleWithoutInstance:
    case Stub::LeafColdPlainOperationDoubleWithoutInstance:
    case Stub::ColdOperationDouble:
    case Stub::LeafColdOperationDouble:
    case Stub::ColdOperationVoidWithoutFPRs:
    case Stub::LeafColdOperationVoidWithoutFPRs:
    case Stub::ColdOperationValueWithoutFPRs:
    case Stub::LeafColdOperationValueWithoutFPRs:
    case Stub::ColdPlainOperationWithoutFPRs:
    case Stub::LeafColdPlainOperationWithoutFPRs:
        return true;
    default:
        return false;
    }
}

GPRReg defaultOperandRegister(Stub stub)
{
    return callsOperation(stub) ? GPRInfo::argumentGPR1 : isHelper(stub) ? GPRInfo::argumentGPR0 : firstStubOperandGPR;
}

bool preservesOperandRegister(Stub stub)
{
    return stub == Stub::WriteBarrier || stub == Stub::PollAtPrologue;
}

std::optional<RegisterSet> registersChangedBy(Stub stub)
{
    if (stub == Stub::AwaitMayContinue || stub == Stub::LeafAwaitMayContinue || stub == Stub::HoldToType || stub == Stub::LeafHoldToType) {
        RegisterSet changed = RegisterSet::macroClobberedGPRs();
        for (GPRReg reg : { T9, T11 })
            changed.add(reg, IgnoreVectors);
        if (isARM64())
            changed.add(T10, IgnoreVectors);
        return changed;
    }
    if (!isARM64() && stub == Stub::TypedNewPromise) {
        RegisterSet changed = RegisterSet::macroClobberedGPRs();
        for (GPRReg reg : stubTemporaryGPRs)
            changed.add(reg, IgnoreVectors);
        return changed;
    }
    if (!isARM64()) {
        auto changes = [](std::initializer_list<GPRReg> registers) {
            RegisterSet changed = RegisterSet::macroClobberedGPRs();
            for (GPRReg reg : registers)
                changed.add(reg, IgnoreVectors);
            return changed;
        };
        if (stub == Stub::ArraySlice || stub == Stub::ArrayCopy)
            return changes({ T9, T10, T11, T12, T13 });
        if (stub >= Stub::AllocateTypedExtension16 && stub <= Stub::AllocateTypedExtension256)
            return changes({ T11, T12 });
        switch (stub) {
        case Stub::TypedStringMadeOnce:
            return changes({ });
        case Stub::RememberElement:
            return changes({ T12 });
        case Stub::LoadElement:
            return changes({ T9, T11, T12, T13, T14 });
        case Stub::CodeUnitOfRope:
            return changes({ T9, T11 });
        case Stub::TypedClosure:
        case Stub::TypedBuiltinCell:
        case Stub::TypedClosureEntry:
            return changes({ T11, T12 });
        case Stub::IsArrayAsMade:
            return changes({ });
        case Stub::AllocateTypedCell:
        case Stub::AllocateTypedObject:
        case Stub::NewTypedClosure0:
        case Stub::NewTypedClosure1:
        case Stub::NewTypedClosure2:
        case Stub::NewTypedClosure3:
        case Stub::NewTypedClosure4:
        case Stub::NewTypedClosureOnce:
        case Stub::NewTypedClosureOnce32:
            return changes({ T11, T12, T13 });
        case Stub::ArrayLiteral0:
        case Stub::ArrayLiteral1:
        case Stub::ArrayLiteral2:
        case Stub::ArrayLiteral3:
        case Stub::ArrayLiteral4:
        case Stub::EmptyArrayWithRoomFor1:
        case Stub::EmptyArrayWithRoomFor3:
        case Stub::EmptyArrayOfDoubles:
        case Stub::EmptyArrayOfInt32s:
        case Stub::EmptyArrayOfDoublesWithRoomFor1:
        case Stub::EmptyArrayOfDoublesWithRoomFor3:
            return changes({ T9, T11, T12 });
        case Stub::ElementOrNothing:
            return changes({ T9, T11, T12, T13 });
        case Stub::PartOfString:
            return changes({ T9, T10, T11, T12, T13 });
        case Stub::TypedString:
        case Stub::StringIsText:
        case Stub::AllocateTypedObjectFromTemplate:
        case Stub::ArrayOfDoubles:
        case Stub::ArrayOfInt32s:
        case Stub::ArrayOfValues:
        case Stub::EmptyArrayWithRoom:
        case Stub::EmptyArrayAtSite:
            return changes({ T9, T11, T12, T13, T14 });
        case Stub::PushToFullVector:
        case Stub::ConcatenateTwo:
        case Stub::ConcatenateThree:
            return changes({ T9, T10, T11, T12, T13 });
        case Stub::StringOfInt32:
            return changes({ T9, T10, GPRInfo::returnValueGPR2 });
        default:
            break;
        }
    }
    unsigned numberOfTemporaries = 0;
    switch (stub) {
    case Stub::PollAtPrologue:
        break;
    case Stub::ColdOperationVoid:
    case Stub::LeafColdOperationVoid:
    case Stub::ColdOperationValue:
    case Stub::LeafColdOperationValue:
    case Stub::ColdPlainOperation:
    case Stub::LeafColdPlainOperation:
    case Stub::ColdPlainOperationWithoutInstance:
    case Stub::LeafColdPlainOperationWithoutInstance:
    case Stub::ColdPlainOperationDouble:
    case Stub::LeafColdPlainOperationDouble:
    case Stub::ColdPlainOperationDoubleWithoutInstance:
    case Stub::LeafColdPlainOperationDoubleWithoutInstance:
    case Stub::ColdOperationDouble:
    case Stub::LeafColdOperationDouble:
    case Stub::ColdOperationVoidWithoutFPRs:
    case Stub::LeafColdOperationVoidWithoutFPRs:
    case Stub::ColdOperationValueWithoutFPRs:
    case Stub::LeafColdOperationValueWithoutFPRs:
    case Stub::ColdPlainOperationWithoutFPRs:
    case Stub::LeafColdPlainOperationWithoutFPRs:
    case Stub::ColdCallFunction:
    case Stub::LeafColdCallFunction:
        numberOfTemporaries = numberOfColdOperationGPR() == T9 ? 2 : 0;
        break;
    case Stub::WriteBarrier:
    case Stub::ToBoolean:
    case Stub::CodeUnitOfRope:
    case Stub::LeafCodeUnitOfRope:
        numberOfTemporaries = 3;
        break;
    case Stub::CharactersOfSubstring:
        if (!isARM64())
            return std::nullopt;
        numberOfTemporaries = 5;
        break;
    case Stub::RememberElement:
    case Stub::LeafRememberElement:
    case Stub::AllocateTypedCell:
    case Stub::LeafAllocateTypedCell:
    case Stub::AllocateTypedObject:
    case Stub::LeafAllocateTypedObject:
    case Stub::AllocateTypedObjectFromTemplate:
    case Stub::LeafAllocateTypedObjectFromTemplate:
    case Stub::NewTypedClosure0:
    case Stub::LeafNewTypedClosure0:
    case Stub::NewTypedClosure1:
    case Stub::LeafNewTypedClosure1:
    case Stub::NewTypedClosure2:
    case Stub::LeafNewTypedClosure2:
    case Stub::NewTypedClosure3:
    case Stub::LeafNewTypedClosure3:
    case Stub::NewTypedClosure4:
    case Stub::LeafNewTypedClosure4:
    case Stub::NewTypedClosureOnce:
    case Stub::LeafNewTypedClosureOnce:
    case Stub::NewTypedClosureOnce32:
    case Stub::LeafNewTypedClosureOnce32:
    case Stub::InitializeTypedClosure0:
    case Stub::LeafInitializeTypedClosure0:
    case Stub::InitializeTypedClosure1:
    case Stub::LeafInitializeTypedClosure1:
    case Stub::InitializeTypedClosure2:
    case Stub::LeafInitializeTypedClosure2:
    case Stub::InitializeTypedClosure3:
    case Stub::LeafInitializeTypedClosure3:
    case Stub::InitializeTypedClosure4:
    case Stub::LeafInitializeTypedClosure4:
    case Stub::MaterializeEnvironment:
    case Stub::LeafMaterializeEnvironment:
    case Stub::TypedString:
    case Stub::LeafTypedString:
    case Stub::TypedStringMadeOnce:
    case Stub::LeafTypedStringMadeOnce:
    case Stub::TypedStringInBlock1:
    case Stub::LeafTypedStringInBlock1:
    case Stub::TypedStringMadeOnceInBlock1:
    case Stub::LeafTypedStringMadeOnceInBlock1:
    case Stub::TypedStringInBlock2:
    case Stub::LeafTypedStringInBlock2:
    case Stub::TypedStringMadeOnceInBlock2:
    case Stub::LeafTypedStringMadeOnceInBlock2:
    case Stub::TypedStringInBlock3:
    case Stub::LeafTypedStringInBlock3:
    case Stub::TypedStringMadeOnceInBlock3:
    case Stub::LeafTypedStringMadeOnceInBlock3:
    case Stub::TypedStringInBlock4:
    case Stub::LeafTypedStringInBlock4:
    case Stub::TypedStringMadeOnceInBlock4:
    case Stub::LeafTypedStringMadeOnceInBlock4:
    case Stub::TypedStringInBlock5:
    case Stub::LeafTypedStringInBlock5:
    case Stub::TypedStringMadeOnceInBlock5:
    case Stub::LeafTypedStringMadeOnceInBlock5:
    case Stub::TypedStringInBlock6:
    case Stub::LeafTypedStringInBlock6:
    case Stub::TypedStringMadeOnceInBlock6:
    case Stub::LeafTypedStringMadeOnceInBlock6:
    case Stub::TypedStringInBlock7:
    case Stub::LeafTypedStringInBlock7:
    case Stub::TypedStringMadeOnceInBlock7:
    case Stub::LeafTypedStringMadeOnceInBlock7:
    case Stub::StringIsText:
    case Stub::LeafStringIsText:
    case Stub::StringStartsWithText:
    case Stub::LeafStringStartsWithText:
    case Stub::StringEndsWithText:
    case Stub::LeafStringEndsWithText:
    case Stub::StringsAreEqual:
    case Stub::LeafStringsAreEqual:
    case Stub::ConcatenateTwo:
    case Stub::LeafConcatenateTwo:
    case Stub::ConcatenateThree:
    case Stub::LeafConcatenateThree:
    case Stub::StringOfInt32:
    case Stub::LeafStringOfInt32:
    case Stub::StringTrim:
    case Stub::LeafStringTrim:
    case Stub::StringTrimStart:
    case Stub::LeafStringTrimStart:
    case Stub::StringTrimEnd:
    case Stub::LeafStringTrimEnd:
    case Stub::StringTrimFlat8:
    case Stub::LeafStringTrimFlat8:
    case Stub::StringTrimStartFlat8:
    case Stub::LeafStringTrimStartFlat8:
    case Stub::StringTrimEndFlat8:
    case Stub::LeafStringTrimEndFlat8:
    case Stub::TypedClosure:
    case Stub::LeafTypedClosure:
    case Stub::TypedBuiltinCell:
    case Stub::LeafTypedBuiltinCell:
    case Stub::TypedClosureEntry:
    case Stub::LeafTypedClosureEntry:
    case Stub::IsArrayAsMade:
    case Stub::LeafIsArrayAsMade:
    case Stub::TypedNewPromise:
    case Stub::LeafTypedNewPromise:
    case Stub::ArrayLiteral0:
    case Stub::LeafArrayLiteral0:
    case Stub::ArrayLiteral1:
    case Stub::LeafArrayLiteral1:
    case Stub::ArrayLiteral2:
    case Stub::LeafArrayLiteral2:
    case Stub::ArrayLiteral3:
    case Stub::LeafArrayLiteral3:
    case Stub::ArrayLiteral4:
    case Stub::LeafArrayLiteral4:
    case Stub::ElementOrNothing:
    case Stub::LeafElementOrNothing:
    case Stub::ArrayOfDoubles:
    case Stub::LeafArrayOfDoubles:
    case Stub::ArrayOfInt32s:
    case Stub::LeafArrayOfInt32s:
    case Stub::EmptyArrayWithRoom:
    case Stub::LeafEmptyArrayWithRoom:
    case Stub::ArrayOfValues:
    case Stub::LeafArrayOfValues:
    case Stub::EmptyArrayOfDoubles:
    case Stub::LeafEmptyArrayOfDoubles:
    case Stub::EmptyArrayOfInt32s:
    case Stub::LeafEmptyArrayOfInt32s:
    case Stub::EmptyArrayOfDoublesWithRoomFor1:
    case Stub::LeafEmptyArrayOfDoublesWithRoomFor1:
    case Stub::EmptyArrayOfDoublesWithRoomFor3:
    case Stub::LeafEmptyArrayOfDoublesWithRoomFor3:
    case Stub::EmptyArrayWithRoomFor1:
    case Stub::LeafEmptyArrayWithRoomFor1:
    case Stub::EmptyArrayWithRoomFor3:
    case Stub::LeafEmptyArrayWithRoomFor3:
    case Stub::EmptyArrayAtSite:
    case Stub::LeafEmptyArrayAtSite:
    case Stub::PushToFullVector:
    case Stub::LeafPushToFullVector:
    case Stub::PushDoubleToFullVector:
    case Stub::LeafPushDoubleToFullVector:
    case Stub::PartOfString:
    case Stub::LeafPartOfString:
    case Stub::StoreElement:
    case Stub::LeafStoreElement:
    case Stub::LoadElement:
    case Stub::LeafLoadElement:
    case Stub::ArraySlice:
    case Stub::LeafArraySlice:
    case Stub::ArrayCopy:
    case Stub::LeafArrayCopy:
        // Elsewhere they are called like any function.
        if (!isARM64())
            return std::nullopt;
        numberOfTemporaries = 7;
        break;
    default:
        return std::nullopt;
    }
    RELEASE_ASSERT(numberOfTemporaries <= std::size(stubTemporaryGPRs));
    RegisterSet result = RegisterSet::macroClobberedGPRs();
    for (unsigned i = 0; i < numberOfTemporaries; ++i)
        result.add(stubTemporaryGPRs[i], IgnoreVectors);
    if (stub == Stub::CharactersOfSubstring)
        result.remove(stubTemporaryGPRs[2]);
    if (stub == Stub::PushDoubleToFullVector || stub == Stub::LeafPushDoubleToFullVector) {
        result.add(A2, IgnoreVectors);
        result.merge(RegisterSet::macroClobberedFPRs());
    }
    switch (stub) {
    case Stub::ColdOperationVoidWithoutFPRs:
    case Stub::LeafColdOperationVoidWithoutFPRs:
    case Stub::ColdOperationValueWithoutFPRs:
    case Stub::LeafColdOperationValueWithoutFPRs:
    case Stub::ColdPlainOperationWithoutFPRs:
    case Stub::LeafColdPlainOperationWithoutFPRs:
    case Stub::StringTrim:
    case Stub::LeafStringTrim:
    case Stub::StringTrimStart:
    case Stub::LeafStringTrimStart:
    case Stub::StringTrimEnd:
    case Stub::LeafStringTrimEnd:
    case Stub::StringTrimFlat8:
    case Stub::LeafStringTrimFlat8:
    case Stub::StringTrimStartFlat8:
    case Stub::LeafStringTrimStartFlat8:
    case Stub::StringTrimEndFlat8:
    case Stub::LeafStringTrimEndFlat8:
        result.merge(RegisterSet::registersToSaveForCCall(RegisterSet::allFPRs()));
        break;
    default:
        break;
    }
    return result;
}

void dumpRegistersChangedByStubs(PrintStream& out)
{
    for (unsigned i = 0; i < numberOfStubs; ++i) {
        Stub stub = static_cast<Stub>(i);
        auto changed = registersChangedBy(stub);
        if (!changed)
            continue;
        bool hasResult = stub != Stub::WriteBarrier && stub != Stub::ColdOperationVoid && stub != Stub::LeafColdOperationVoid && stub != Stub::ColdOperationVoidWithoutFPRs && stub != Stub::LeafColdOperationVoidWithoutFPRs
            && stub != Stub::ColdCallFunction && stub != Stub::LeafColdCallFunction && stub != Stub::PollAtPrologue;
        if (hasResult)
            changed->add(GPRInfo::returnValueGPR, IgnoreVectors);
        bool returnsDouble = stub == Stub::ColdOperationDouble || stub == Stub::LeafColdOperationDouble || stub == Stub::ColdPlainOperationDouble || stub == Stub::LeafColdPlainOperationDouble
            || stub == Stub::ColdPlainOperationDoubleWithoutInstance || stub == Stub::LeafColdPlainOperationDoubleWithoutInstance;
        if (returnsDouble)
            changed->add(FPRInfo::returnValueFPR, IgnoreVectors);
        out.print("K\t", nameOf(stub), "\t");
        changed->forEach([&](Reg reg) {
            out.print(reg, " ");
        });
        out.print("\t");
        if (!preservesOperandRegister(stub))
            out.print(defaultOperandRegister(stub));
        out.println();
    }
}

std::optional<unsigned> thunkFor(Stub stub, uint32_t t9Value)
{
    for (unsigned i = 0; i < std::size(operationCallStubs); ++i) {
        if (operationCallStubs[i] == stub)
            return t9Value % sizeof(void*) || t9Value / sizeof(void*) >= numberOfEntries ? std::nullopt : std::optional<unsigned> { i * numberOfEntries + t9Value / sizeof(void*) };
    }
    for (unsigned i = 0; i < std::size(functionCallStubs); ++i) {
        if (functionCallStubs[i] == stub)
            return t9Value >= numberOfCountsWithThunk ? std::nullopt : std::optional<unsigned> { firstCallThunk + i * numberOfCountsWithThunk + t9Value };
    }
    return std::nullopt;
}

// `o.m(..)`: see Typed::Lowering::callMethodThroughSlot() for what stands at the site. thisGPR: the object. calleeGPR: the address of the slot. The arguments are where the call takes them.
static constexpr unsigned bytesSavedForCallMethod(unsigned count) { return WTF::roundUpToMultipleOf<stackAlignmentBytes()>((count + 2) * sizeof(EncodedJSValue)); }

static void generateCallMethod(CCallHelpers& jit, unsigned count, CCallHelpers::Label callValue)
{
    constexpr GPRReg slot = calleeGPR;
    constexpr GPRReg word = T11;
    constexpr GPRReg storage = T13;
#if CPU(ARM64)
    // T12 is branchIfStructureDiffers()'s.
    constexpr GPRReg holder = T14;
    static_assert(noOverlap(thisGPR, slot, word, T12, storage, holder));
#else
    constexpr GPRReg holder = T9;
    static_assert(noOverlap(thisGPR, slot, word, storage, holder));
    static_assert(thisGPR == R0);
#endif
    for (unsigned i = 0; i < numberOfArgumentGPRs; ++i)
        RELEASE_ASSERT(noOverlap(argumentGPR(i), thisGPR, slot, word, storage, holder) && (!isARM64() || argumentGPR(i) != T12));

    // As Stub::GetById reads, where it reads a value and calls nothing: with three temporaries, so that what the call takes stays where it is.
    CCallHelpers::JumpList otherwise;
    CCallHelpers::JumpList isRead;
    // The first word of the slot is in `word`, and the object has the structure it says.
    auto readThrough = [&](GPRReg hit) {
        Jump isIndirect = branchIfSlotHas<Slot::isIndirect>(jit, word);
        loadDirectLocation(jit, hit, word, word);
        jit.load64(CCallHelpers::BaseIndex(thisGPR, word, CCallHelpers::TimesEight), word);
        isRead.append(jit.jump());
        isIndirect.link(&jit);
        otherwise.append(branchIfSlotHas<Slot::isGetter>(jit, word));
        jit.loadPtr(slotWord(hit, 1), holder);
        jit.moveConditionallyTest64(CCallHelpers::NonZero, holder, holder, holder, thisGPR, holder);
        locateCachedProperty(jit, holder, word, storage);
        jit.load64(CCallHelpers::BaseIndex(storage, word, CCallHelpers::TimesEight), word);
        isRead.append(jit.jump());
    };
    otherwise.append(jit.branchIfNotCell(thisGPR));
    jit.load64(slotWord(slot, 0), word);
    Jump hasDifferentStructure = branchIfStructureDiffers(jit, thisGPR, word);
    readThrough(slot);

    hasDifferentStructure.link(&jit);
    Jump standsForSeveral = branchIfSlotHas<Slot::isIndirect>(jit, word);
    // readByName(): another structure that has the name in the same place.
    loadDirectLocation(jit, slot, word, storage);
    jit.urshift64(TrustedImm32(32 + Slot::nameIDShift), word);
    otherwise.append(jit.branchTest64(CCallHelpers::Zero, word));
    jit.load32(Address(thisGPR, JSCell::structureIDOffset()), holder);
    jit.add64(Address(instanceGPR, Instance::offsetOfFieldIDAtDirectLocationBase()), holder);
    jit.load16(CCallHelpers::BaseIndex(holder, storage, CCallHelpers::TimesTwo), holder);
    otherwise.append(jit.branch32(CCallHelpers::NotEqual, holder, word));
    jit.load64(CCallHelpers::BaseIndex(thisGPR, storage, CCallHelpers::TimesEight), word);
    isRead.append(jit.jump());

    // No structure, and indirect: of such slots only one that stands for several has the flag. The one slot that the structure can have there: PolymorphicSlots::indexFor().
    standsForSeveral.link(&jit);
    static_assert(Slot::polymorphicFlags == (Slot::isIndirect | Slot::pointerIsNotCell));
    static_assert(sizeof(Slot) == 16 && PolymorphicSlots::structureShift >= 4);
    otherwise.append(jit.branchTest32(CCallHelpers::NonZero, word));
    Jump isSeveral = branchIfSlotHas<Slot::pointerIsNotCell>(jit, word);
    otherwise.append(jit.jump());
    isSeveral.link(&jit);
    jit.loadPtr(slotWord(slot, 1), storage);
    jit.load32(Address(thisGPR, JSCell::structureIDOffset()), holder);
    jit.and32(TrustedImm32((PolymorphicSlots::numberOfSlots - 1) << PolymorphicSlots::structureShift), holder, word);
    jit.urshift32(TrustedImm32(PolymorphicSlots::structureShift - 4), word);
    jit.addPtr(word, storage);
    jit.addPtr(TrustedImm32(PolymorphicSlots::offsetOfSlots()), storage);
    jit.load64(slotWord(storage, 0), word);
    otherwise.append(jit.branch32(CCallHelpers::NotEqual, word, holder));
    readThrough(storage);

    isRead.link(&jit);
    otherwise.append(jit.branchIfNotCell(word));
    jit.move(word, calleeGPR);
    jit.jump().linkTo(callValue, &jit);

    otherwise.link(&jit);
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(bytesSavedForCallMethod(count)), sp);
    jit.store64(slot, Address(sp));
    jit.store64(thisGPR, Address(sp, sizeof(EncodedJSValue)));
    for (unsigned i = 0; i < count; ++i)
        jit.store64(argumentGPR(i), Address(sp, (i + 2) * sizeof(EncodedJSValue)));
    jit.move(slot, A1);
    jit.move(thisGPR, R0);
    callStubFromStub(jit, Stub::CallMethodRead);
    Jump isNoCell = jit.branchIfNotCell(R0);
    jit.move(R0, calleeGPR);
    jit.load64(Address(sp, sizeof(EncodedJSValue)), thisGPR);
    for (unsigned i = 0; i < count; ++i)
        jit.load64(Address(sp, (i + 2) * sizeof(EncodedJSValue)), argumentGPR(i));
    jit.emitFunctionEpilogue();
    jit.jump().linkTo(callValue, &jit);

    isNoCell.link(&jit);
    jit.loadPtr(Address(sp), A1);
    jit.emitFunctionEpilogue();
    prepareMissAtSite(jit, Entry::operationAOTTypedThrowMethodIsNotAFunction, 1);
    callAndCheckException(jit, T9, Returns::Void);
}

const StubBlob& stubBlob(unsigned codeGranuleShift, bool functionLengthsAreBytes)
{
    RELEASE_ASSERT(codeGranuleShift >= minCodeGranuleShift && codeGranuleShift <= maxCodeGranuleShift);
    static LazyNeverDestroyed<StubBlob> blobs[2][maxCodeGranuleShift - minCodeGranuleShift + 1];
    static std::once_flag flags[2][maxCodeGranuleShift - minCodeGranuleShift + 1];
    auto& blob = blobs[functionLengthsAreBytes][codeGranuleShift - minCodeGranuleShift];
    static Lock lock;
    Locker locker { lock };
    std::call_once(flags[functionLengthsAreBytes][codeGranuleShift - minCodeGranuleShift], [&] {
        s_codeGranuleShift = codeGranuleShift;
        s_functionLengthsAreBytes = functionLengthsAreBytes;
        blob.construct();
        CCallHelpers jit;
#if CPU(ARM64) || CPU(X86_64)
        CCallHelpers::Label* labels = stubLabels();
        Vector<std::pair<CCallHelpers::Call, Stub>> callsBetweenStubs;
        s_callsBetweenStubs = &callsBetweenStubs;
        Vector<CCallHelpers::Label> returnsIntoAdapters;
        s_returnsIntoAdapters = &returnsIntoAdapters;
        Vector<CCallHelpers::Label> returnsFromFindingTargets;
        s_returnsFromFindingTargets = &returnsFromFindingTargets;
#if CPU(ARM64)
        Vector<LabelAddress> labelAddresses;
        s_labelAddresses = &labelAddresses;
#endif
#else
        Vector<CCallHelpers::Label> returnsIntoAdapters;
        Vector<CCallHelpers::Label> returnsFromFindingTargets;
        CCallHelpers::Label labels[numberOfStubs];
        Vector<std::pair<CCallHelpers::Call, Stub>> callsBetweenStubs;
#endif
        for (unsigned i = 0; i < numberOfStubs; ++i) {
            jit.align();
            labels[i] = jit.label();
            generateStub(jit, static_cast<Stub>(i));
        }

        Vector<CCallHelpers::Label> thunkLabels;
#if CPU(ARM64) || CPU(X86_64)
        {
            for (Stub stub : operationCallStubs) {
                for (unsigned entry = 0; entry < numberOfEntries; ++entry) {
                    thunkLabels.append(jit.label());
#if CPU(ARM64)
                    if (entry == static_cast<unsigned>(Entry::operationAOTTypedBarrierSlowKeepingRegisters) && generateBarrierKeepingRegisters(jit, stub))
                        continue;
#endif
                    if (entry == static_cast<unsigned>(Entry::operationAOTTypedBarrierSlow))
                        returnFromBarrierUnlessOwnerIsBlack(jit, stub);
                    jit.move(CCallHelpers::TrustedImm32(entry * sizeof(void*)), isColdOperationWithThunk(stub) ? numberOfColdOperationGPR() : T9);
                    jit.jump().linkTo(labels[static_cast<unsigned>(stub)], &jit);
                }
            }
            Vector<std::pair<Jump, unsigned>> jumpsToCallValue = std::exchange(jumpsFromStubsToCallValue(), { });
            Vector<std::pair<Jump, unsigned>> jumpsToCallMethod = std::exchange(jumpsFromStubsToCallMethod(), { });
            Vector<CCallHelpers::Label, numberOfCountsWithThunk> labelsAheadOfCallValue;
            Vector<CCallHelpers::Label, numberOfCountsWithThunk> labelsOfNoClosureInCallValue;
            for (Stub stub : functionCallStubs) {
                for (unsigned count = 0; count < numberOfCountsWithThunk; ++count) {
                    if (stub == Stub::CallValueWithoutThis) {
                        thunkLabels.append(labelsAheadOfCallValue[count]);
                        continue;
                    }
                    jit.align();
                    if (stub == Stub::CallValue) {
                        constexpr unsigned sizeOfMove = isX86_64() ? 5 : 4;
                        while ((jit.m_assembler.codeSize() + sizeOfMove) % 16)
                            jit.nop();
                        labelsAheadOfCallValue.append(jit.label());
                        jit.move(CCallHelpers::TrustedImm64(JSValue::encode(jsUndefined())), thisGPR);
                        RELEASE_ASSERT(!(jit.m_assembler.codeSize() % 16));
                    }
                    thunkLabels.append(jit.label());
                    if (stub == Stub::CallHostFunction) {
                        callHostFunction(jit, count);
                        continue;
                    }
                    if (stub == Stub::CallInternalFunction || stub == Stub::ConstructInternalFunction) {
                        callThroughTrampoline(jit, count, stub == Stub::CallInternalFunction ? Entry::InternalFunctionCallTrampoline : Entry::InternalFunctionConstructTrampoline);
                        continue;
                    }
                    // The usual callee: a closure with the structure it was allocated with, whose entry point takes its arguments in registers, and no more of them than are passed.
                    if (stub == Stub::CallValue) {
                        for (auto& [jump, target] : jumpsToCallValue) {
                            if (target == count)
                                jump.linkTo(thunkLabels.last(), &jit);
                        }
                        CCallHelpers::JumpList callGenerically;
                        callGenerically.append(jit.branchIfNotCell(calleeGPR));
                        jit.load32(Address(calleeGPR, JSCell::structureIDOffset()), T11);
                        auto [isSlim, isNeither] = branchIfNotClosureStructure(jit, T11, T13);
                        jit.loadPtr(Address(calleeGPR, Typed::Closure::offsetOfEntry()), T11);
                        enterClosure(jit, count, callGenerically);
                        isSlim.link(&jit);
                        jit.loadPtr(Address(calleeGPR, Typed::SlimClosure::offsetOfWord()), T11);
                        enterClosure(jit, count, callGenerically);
                        callGenerically.link(&jit);
                        jit.jump().linkTo(thunkLabels[firstCallThunkOf(Stub::Call) + count], &jit);

                        isNeither.link(&jit);
                        labelsOfNoClosureInCallValue.append(jit.label());
                        jit.load8(Address(calleeGPR, JSCell::typeInfoTypeOffset()), T13);
                        jit.branch32(CCallHelpers::NotEqual, T13, TrustedImm32(JSFunctionType)).linkTo(callNoJSFunctionLabel(count), &jit);
                        structureWithID(jit, T11);
                        jit.branchPtr(CCallHelpers::Equal, Address(T11, Structure::offsetOfAOTInstance()), instanceGPR).linkTo(callJSFunctionOfThisInstanceLabel(count), &jit);
                        static_assert(sizeof(StructureID) == sizeof(uint32_t) && StructureID::structureIDMask == UINT_MAX);
                        jit.branch32(CCallHelpers::Equal, T11, Address(instanceGPR, Instance::offsetOfHostFunctionStructureID())).linkTo(thunkLabels[firstCallThunkOf(Stub::CallHostFunction) + count], &jit);
                        Jump isNotBoundFunction = jit.branch32(CCallHelpers::NotEqual, T11, Address(instanceGPR, Instance::offsetOfBoundFunctionStructureID()));
                        CCallHelpers::Label isBoundFunction = jit.label();
                        auto callTarget = [&](unsigned numberOfArguments) {
                            jit.load32(Address(calleeGPR, JSCell::structureIDOffset()), T11);
                            jit.branch32(CCallHelpers::Equal, T11, Address(instanceGPR, Instance::offsetOfHostFunctionStructureID())).linkTo(thunkLabels[firstCallThunkOf(Stub::CallHostFunction) + numberOfArguments], &jit);
                            if (numberOfArguments == count)
                                jit.jump().linkTo(thunkLabels.last(), &jit);
                            else
                                jumpsToCallValue.append({ jit.jump(), numberOfArguments });
                        };
                        jit.load32(Address(calleeGPR, JSBoundFunction::offsetOfBoundArgsLength()), T13);
                        Jump hasBoundArguments = jit.branchTest32(CCallHelpers::NonZero, T13);
                        jit.loadPtr(Address(calleeGPR, JSBoundFunction::offsetOfTargetFunction()), T13);
                        jit.branchPtr(CCallHelpers::Equal, T13, Address(instanceGPR, Instance::offsetOfFunctionPrototypeCall())).linkTo(callNoClosureLabel(count), &jit);
                        jit.load64(Address(calleeGPR, JSBoundFunction::offsetOfBoundThis()), thisGPR);
                        jit.move(T13, calleeGPR);
                        callTarget(count);
                        hasBoundArguments.link(&jit);
                        for (unsigned bound = 1; bound <= JSBoundFunction::maxEmbeddedArgs && count + bound <= numberOfArgumentGPRs; ++bound) {
                            Jump hasMore = jit.branch32(CCallHelpers::NotEqual, T13, TrustedImm32(bound));
                            for (unsigned i = count; i--;)
                                jit.move(argumentGPR(i), argumentGPR(i + bound));
                            for (unsigned i = 0; i < bound; ++i)
                                jit.load64(Address(calleeGPR, JSBoundFunction::offsetOfBoundArgs() + i * sizeof(WriteBarrier<Unknown>)), argumentGPR(i));
                            jit.load64(Address(calleeGPR, JSBoundFunction::offsetOfBoundThis()), thisGPR);
                            jit.loadPtr(Address(calleeGPR, JSBoundFunction::offsetOfTargetFunction()), calleeGPR);
                            callTarget(count + bound);
                            hasMore.link(&jit);
                        }
                        jit.jump().linkTo(callNoClosureLabel(count), &jit);

                        isNotBoundFunction.link(&jit);
                        jit.loadPtr(Address(T11, Structure::classInfoOffset()), T13);
                        jit.branchPtr(CCallHelpers::Equal, T13, Address(instanceGPR, Instance::offsetOfBoundFunctionClassInfo())).linkTo(isBoundFunction, &jit);
                        jit.loadPtr(Address(calleeGPR, JSFunction::offsetOfExecutableOrRareData()), T13);
                        Jump isTagged = jit.branchTestPtr(CCallHelpers::NonZero, T13, TrustedImm32(JSFunction::rareDataTag));
                        jit.branchIfType(T13, NativeExecutableType).linkTo(thunkLabels[firstCallThunkOf(Stub::CallHostFunction) + count], &jit);
                        jit.jump().linkTo(callNoClosureLabel(count), &jit);
                        isTagged.link(&jit);
                        jit.loadPtr(Address(T13, FunctionRareData::offsetOfExecutable() - JSFunction::rareDataTag), T13);
                        jit.branchIfType(T13, NativeExecutableType).linkTo(thunkLabels[firstCallThunkOf(Stub::CallHostFunction) + count], &jit);
                        jit.jump().linkTo(callNoClosureLabel(count), &jit);
                        continue;
                    }
                    if (stub == Stub::ConstructClosure) {
                        CCallHelpers::JumpList never;
                        jit.and64(CCallHelpers::TrustedImm64(static_cast<int64_t>(~(1ULL << ClosureWord::isNotCallableBit))), T11);
                        enterClosure(jit, count, never);
                        never.link(&jit);
                        jit.breakpoint();
                        continue;
                    }
                    if (stub == Stub::CallValueNamed) {
                        CCallHelpers::JumpList callGenerically;
                        Jump isNoCell = jit.branchIfNotCell(calleeGPR);
                        jit.load32(Address(calleeGPR, JSCell::structureIDOffset()), T11);
                        auto [isSlim, isNeither] = branchIfNotClosureStructure(jit, T11, T13);
                        jit.loadPtr(Address(calleeGPR, Typed::Closure::offsetOfEntry()), T11);
                        enterClosure(jit, count, callGenerically);
                        isSlim.link(&jit);
                        jit.loadPtr(Address(calleeGPR, Typed::SlimClosure::offsetOfWord()), T11);
                        enterClosure(jit, count, callGenerically);
                        callGenerically.link(&jit);
                        jit.jump().linkTo(thunkLabels[firstCallThunkOf(Stub::Call) + count], &jit);
                        isNeither.linkTo(labelsOfNoClosureInCallValue[count], &jit);
                        isNoCell.link(&jit);
                        jit.move(T11, throwWordGPR);
                        jit.jump().linkTo(labels[static_cast<unsigned>(Stub::ThrowAtSiteByWord)], &jit);
                        continue;
                    }
                    if (stub == Stub::CallMethod) {
                        for (auto& [jump, target] : jumpsToCallMethod) {
                            if (target == count)
                                jump.linkTo(thunkLabels.last(), &jit);
                        }
                        generateCallMethod(jit, count, thunkLabels[firstCallThunkOf(Stub::CallValue) + count]);
                        continue;
                    }
                    generateCallTo(jit, CodeSpecializationKind::CodeForCall, count);
                }
            }
            RELEASE_ASSERT(thunkLabels.size() == numberOfThunks);
        }
#endif

        jit.padBeforePatch();
        Vector<uint32_t> storage(WTF::roundUpToMultipleOf<sizeof(uint32_t)>(jit.m_assembler.codeSize()) / sizeof(uint32_t));
        LinkBuffer linkBuffer(jit, CodePtr<LinkBufferPtrTag>::fromUntaggedPtr(storage.mutableSpan().data()), storage.sizeInBytes(), LinkBuffer::Profile::Thunk);
        for (auto& [call, stub] : callsBetweenStubs)
            linkBuffer.link<JITThunkPtrTag>(call, linkBuffer.locationOf<JITThunkPtrTag>(labels[static_cast<unsigned>(stub)]));
#if CPU(ARM64)
        for (auto& address : labelAddresses) {
            auto* instruction = static_cast<uint8_t*>(linkBuffer.locationOf<JITThunkPtrTag>(address.instruction).untaggedPtr());
            int64_t delta = static_cast<uint8_t*>(linkBuffer.locationOf<JITThunkPtrTag>(address.target).untaggedPtr()) - instruction;
            RELEASE_ASSERT(delta >= -(1 << 20) && delta < (1 << 20));
            uint32_t encoded = 0x10000000u | (static_cast<uint32_t>(delta) & 3) << 29 | (static_cast<uint32_t>(delta >> 2) & 0x7ffff) << 5 | static_cast<uint32_t>(address.reg);
            performJITMemcpy<jitMemcpyRepatch>(instruction, &encoded, sizeof(encoded));
        }
#endif
        auto* start = static_cast<uint8_t*>(linkBuffer.entrypoint<JITThunkPtrTag>().untaggedPtr());
        for (unsigned i = 0; i < numberOfStubs; ++i)
            blob->offsets[i] = static_cast<uint8_t*>(linkBuffer.locationOf<JITThunkPtrTag>(labels[i]).untaggedPtr()) - start;
        for (auto& label : returnsIntoAdapters)
            blob->returnsIntoAdapters.append(static_cast<uint8_t*>(linkBuffer.locationOf<JITThunkPtrTag>(label).untaggedPtr()) - start);
        for (auto& label : returnsFromFindingTargets)
            blob->returnsFromFindingTargets.append(static_cast<uint8_t*>(linkBuffer.locationOf<JITThunkPtrTag>(label).untaggedPtr()) - start);
        for (auto& label : thunkLabels)
            blob->thunkOffsets.append(static_cast<uint8_t*>(linkBuffer.locationOf<JITThunkPtrTag>(label).untaggedPtr()) - start);
        size_t size = linkBuffer.size();
        MacroAssemblerCodeRef<JITThunkPtrTag> code = FINALIZE_THUNK(linkBuffer, JITThunkPtrTag, "AOTStubs"_s, "AOT stubs");
#if CPU(ARM64)
        while (size > sizeof(uint32_t) && *reinterpret_cast<const uint32_t*>(start + size - sizeof(uint32_t)) == 0xd503201f)
            size -= sizeof(uint32_t);
#endif
        blob->bytes.append(std::span { start, size });
        if (Options::verboseAOTCompilation() || Options::aotMapFilePath()) [[unlikely]] {
            static constexpr ASCIILiteral names[] = {
#define AOT_STUB_NAME(name) #name ""_s,
                FOR_EACH_AOT_STUB(AOT_STUB_NAME)
#undef AOT_STUB_NAME
            };
            static constexpr ASCIILiteral entryNames[] = {
#define AOT_ENTRY_NAME(name) #name ""_s,
                FOR_EACH_AOT_OPERATION(AOT_ENTRY_NAME)
                FOR_EACH_AOT_THUNK(AOT_ENTRY_NAME)
                FOR_EACH_AOT_POINTER(AOT_ENTRY_NAME)
#undef AOT_ENTRY_NAME
            };
            auto nameOf = [&](Stub stub) { return names[static_cast<unsigned>(stub)]; };
            auto note = [&](unsigned offset, auto... parts) { blob->names.append({ makeString(parts...), offset }); };
            for (unsigned i = 0; i < numberOfStubs; ++i)
                note(blob->offsets[i], names[i]);
            unsigned thunk = 0;
            for (Stub stub : operationCallStubs) {
                if (blob->thunkOffsets.isEmpty())
                    break;
                for (unsigned entry = 0; entry < numberOfEntries; ++entry)
                    note(blob->thunkOffsets[thunk++], nameOf(stub), " to "_s, entryNames[entry]);
            }
            for (Stub stub : functionCallStubs) {
                if (blob->thunkOffsets.isEmpty())
                    break;
                for (unsigned count = 0; count < numberOfCountsWithThunk; ++count)
                    note(blob->thunkOffsets[thunk++], nameOf(stub), count);
            }
            note(size, "End"_s);
            if (Options::verboseAOTCompilation()) {
                for (auto& [name, offset] : blob->names)
                    dataLogLn("AOT: stub ", offset, " ", name);
            }
        }
    });
    return blob.get();
}

#if CPU(ARM64) || CPU(X86_64)

void StubCalls::call(CCallHelpers& jit, Stub stub, CallSite site)
{
    m_pending.append({ jit.nearCall(), stub, false, site.bits });
}

void StubCalls::call(CCallHelpers& jit, Stub stub, uint32_t t9Value, CallSite site)
{
    auto thunk = thunkFor(stub, t9Value);
    RELEASE_ASSERT(thunk || !isColdOperationWithThunk(stub));
    if (!thunk)
        jit.move(CCallHelpers::TrustedImm32(t9Value), stubImmediateGPR);
    m_pending.append({ jit.nearCall(), stub, false, site.bits });
    if (thunk)
        m_pending.last().thunk = safeCast<uint16_t>(*thunk + 1);
}

void StubCalls::call(CCallHelpers& jit, Stub stub, uint32_t t9Value, CallSite site, const ArgumentMoves& moves)
{
    RELEASE_ASSERT(thunkFor(stub, t9Value) && !moves.isEmpty());
    m_pending.append({ jit.nearCall(), stub, false, site.bits });
    m_pending.last().argumentMoves = m_argumentMoves.size();
    ArgumentMoves words;
    words.append(static_cast<uint64_t>(stub));
    words.append(t9Value);
    words.appendVector(moves);
    m_argumentMoves.append(WTF::move(words));
}

void StubCalls::call(CCallHelpers& jit, Stub stub, CallSite site, const ArgumentMoves& moves)
{
    RELEASE_ASSERT(!moves.isEmpty());
    m_pending.append({ jit.nearCall(), stub, false, site.bits });
    m_pending.last().argumentMoves = m_argumentMoves.size();
    ArgumentMoves words;
    words.append(static_cast<uint64_t>(stub));
    words.append(StubCall::noImmediate);
    words.appendVector(moves);
    m_argumentMoves.append(WTF::move(words));
}

void StubCalls::tailCall(CCallHelpers& jit, Stub stub)
{
    m_pending.append({ jit.nearTailCall(), stub, true, StubCall::noCallSite });
}

void StubCalls::tailCall(CCallHelpers& jit, Stub stub, uint32_t t9Value)
{
    auto thunk = thunkFor(stub, t9Value);
    RELEASE_ASSERT(thunk || !isColdOperationWithThunk(stub));
    if (!thunk)
        jit.move(CCallHelpers::TrustedImm32(t9Value), stubImmediateGPR);
    m_pending.append({ jit.nearTailCall(), stub, true, StubCall::noCallSite });
    if (thunk)
        m_pending.last().thunk = safeCast<uint16_t>(*thunk + 1);
}

void StubCalls::callFunction(CCallHelpers& jit, uint32_t knownCallee, CallSite site, uint8_t firstOmittedGPR, uint8_t endOfOmittedGPRs)
{
    m_pending.append({ jit.nearCall(), Stub::Call, false, site.bits, knownCallee, 0, StubCall::noArgumentMoves, firstOmittedGPR, endOfOmittedGPRs });
}

void StubCalls::jumpToFunction(CCallHelpers& jit, uint32_t knownCallee)
{
    m_pending.append({ jit.nearTailCall(), Stub::Call, true, StubCall::noCallSite, knownCallee });
}

ArgumentThunks generateArgumentThunks(const Vector<const ArgumentMoves*>& all)
{
    ArgumentThunks result;
    if (all.isEmpty())
        return result;
    CCallHelpers jit;
    AllowMacroScratchRegisterUsage allowScratch(jit);
    StubCalls jumps;
    Vector<CCallHelpers::Label> labels;
    const char* breakForTesting = byteCast<char>(Options::aotSlowPathFaultForTesting());
    for (auto* moves : all) {
        auto& words = *moves;
        labels.append(jit.label());
        Vector<std::pair<GPRReg, GPRReg>, 8> pending;
        for (size_t i = 2; i < words.size(); i += 2) {
            if (static_cast<ArgumentSource>(words[i] & 0xff) == ArgumentSource::Register)
                pending.append({ static_cast<GPRReg>(words[i + 1]), static_cast<GPRReg>(words[i] >> 8) });
        }
        if (breakForTesting && !strcmp(breakForTesting, "register") && !pending.isEmpty())
            pending.removeLast();
        if (breakForTesting && !strcmp(breakForTesting, "order")) {
            for (auto& move : pending)
                jit.move(move.first, move.second);
            pending.clear();
        }
        int64_t wrongConstant = breakForTesting && !strcmp(breakForTesting, "constant");
        int32_t wrongSlot = breakForTesting && !strcmp(breakForTesting, "stack") ? 8 : 0;
        while (!pending.isEmpty()) {
            size_t free = pending.findIf([&](auto& move) {
                return !pending.containsIf([&](auto& other) {
                    return other.first == move.second;
                });
            });
            if (free != notFound) {
                jit.move(pending[free].first, pending[free].second);
                pending.removeAt(free);
                continue;
            }
            auto [source, destination] = pending.takeLast();
            jit.swap(source, destination);
            for (auto& other : pending) {
                if (other.first == destination)
                    other.first = source;
                else if (other.first == source)
                    other.first = destination;
            }
            pending.removeAllMatching([](auto& move) {
                return move.first == move.second;
            });
        }
        for (size_t i = 2; i < words.size(); i += 2) {
            GPRReg destination = static_cast<GPRReg>(words[i] >> 8);
            switch (static_cast<ArgumentSource>(words[i] & 0xff)) {
            case ArgumentSource::Register:
                break;
            case ArgumentSource::Stack32:
                jit.load32(CCallHelpers::Address(GPRInfo::callFrameRegister, static_cast<int32_t>(words[i + 1]) + wrongSlot), destination);
                break;
            case ArgumentSource::Stack64:
                jit.load64(CCallHelpers::Address(GPRInfo::callFrameRegister, static_cast<int32_t>(words[i + 1]) + wrongSlot), destination);
                break;
            case ArgumentSource::Constant:
                jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(words[i + 1]) + wrongConstant), destination);
                break;
            }
        }
        if (words[1] == StubCall::noImmediate)
            jumps.tailCall(jit, static_cast<Stub>(words[0]));
        else
            jumps.tailCall(jit, static_cast<Stub>(words[0]), static_cast<uint32_t>(words[1]));
    }
    jit.padBeforePatch();
    Vector<uint32_t> storage(WTF::roundUpToMultipleOf<sizeof(uint32_t)>(jit.m_assembler.codeSize()) / sizeof(uint32_t));
    LinkBuffer linkBuffer(jit, CodePtr<LinkBufferPtrTag>::fromUntaggedPtr(storage.mutableSpan().data()), storage.sizeInBytes(), LinkBuffer::Profile::Thunk);
    result.jumps = jumps.link(linkBuffer);
    auto* start = static_cast<uint8_t*>(linkBuffer.entrypoint<JITThunkPtrTag>().untaggedPtr());
    for (auto& label : labels)
        result.offsets.append(static_cast<uint8_t*>(linkBuffer.locationOf<JITThunkPtrTag>(label).untaggedPtr()) - start);
    size_t size = linkBuffer.size();
    MacroAssemblerCodeRef<JITThunkPtrTag> code = FINALIZE_THUNK(linkBuffer, JITThunkPtrTag, "AOTArgumentThunks"_s, "AOT argument thunks");
    result.bytes.append(std::span { start, size });
    return result;
}

#endif // CPU(ARM64) || CPU(X86_64)

void IndexReferences::load(CCallHelpers& jit, GPRReg base, GPRReg dest, uint32_t addend, uint32_t scale)
{
    bool isHalfWord = scale == sizeof(uint32_t);
    RELEASE_ASSERT(isHalfWord ? !(addend % sizeof(uint32_t)) : !(addend % sizeof(void*)) && !(scale % sizeof(void*)));
    RELEASE_ASSERT(scale <= std::numeric_limits<uint16_t>::max());
#if CPU(ARM64)
    m_references.append({ jit.label(), addend, scale });
    jit.m_assembler.add<64>(dest, base, UInt12(0), 12);
    if (isHalfWord)
        jit.m_assembler.ldr<32>(dest, dest, 0u);
    else
        jit.m_assembler.ldr<64>(dest, dest, 0u);
#elif CPU(X86_64)
    if (isHalfWord)
        jit.m_assembler.movl_mr_disp32(0, base, dest);
    else
        jit.m_assembler.movq_mr_disp32(0, base, dest);
    m_references.append({ jit.label(), addend, scale });
#else
    UNUSED_PARAM(base);
    UNUSED_PARAM(dest);
    RELEASE_ASSERT_NOT_REACHED();
#endif
}

void IndexReferences::load16(CCallHelpers& jit, GPRReg base, GPRReg dest, uint32_t addend, uint32_t scale)
{
    RELEASE_ASSERT(!(addend % sizeof(uint16_t)) && !(scale % sizeof(uint16_t)) && scale <= std::numeric_limits<uint16_t>::max());
#if CPU(ARM64)
    m_references.append({ jit.label(), addend, scale });
    jit.m_assembler.add<64>(dest, base, UInt12(0), 12);
    jit.m_assembler.ldrh(dest, dest, 0u);
#elif CPU(X86_64)
    jit.m_assembler.movzwl_mr(std::numeric_limits<int32_t>::max(), base, dest);
    m_references.append({ jit.label(), addend, scale });
#else
    UNUSED_PARAM(base);
    UNUSED_PARAM(dest);
    RELEASE_ASSERT_NOT_REACHED();
#endif
}

Vector<IndexReference> IndexReferences::link(LinkBuffer& linkBuffer)
{
    auto* start = static_cast<uint8_t*>(linkBuffer.entrypoint<JSEntryPtrTag>().untaggedPtr());
    Vector<IndexReference> result;
    for (auto& reference : m_references)
        result.append({ static_cast<uint32_t>(static_cast<uint8_t*>(linkBuffer.locationOf<JSEntryPtrTag>(reference.instructions).untaggedPtr()) - start), reference.addend, static_cast<uint16_t>(reference.scale) });
    return result;
}

void IndexReferences::fill(uint8_t* code, const IndexReference& reference, uint32_t index)
{
#if CPU(ARM64)
    uint64_t distance = reference.addend + static_cast<uint64_t>(index) * reference.scale;
    RELEASE_ASSERT(distance < (1u << 24));
    auto* instructions = reinterpret_cast<uint32_t*>(code + reference.offset);
    constexpr uint32_t immediate = 0xfffu << 10;
    RELEASE_ASSERT(!(instructions[0] & immediate) && !(instructions[1] & immediate));
    instructions[0] |= static_cast<uint32_t>(distance >> 12) << 10;
    unsigned logOfBytesLoaded = instructions[1] >> 30;
    RELEASE_ASSERT(!(distance & ((1u << logOfBytesLoaded) - 1)));
    instructions[1] |= static_cast<uint32_t>((distance & 0xfff) >> logOfBytesLoaded) << 10;
#elif CPU(X86_64)
    uint64_t distance = reference.addend + static_cast<uint64_t>(index) * reference.scale;
    RELEASE_ASSERT(distance <= static_cast<uint64_t>(std::numeric_limits<int32_t>::max()));
    uint8_t* displacement = code + reference.offset - sizeof(int32_t);
    int32_t encoded = 0;
    memcpy(&encoded, displacement, sizeof(encoded));
    RELEASE_ASSERT(!encoded || encoded == std::numeric_limits<int32_t>::max());
    encoded = static_cast<int32_t>(distance);
    memcpy(displacement, &encoded, sizeof(encoded));
#else
    UNUSED_PARAM(code);
    UNUSED_PARAM(reference);
    UNUSED_PARAM(index);
    RELEASE_ASSERT_NOT_REACHED();
#endif
}

Vector<StubCall> StubCalls::link(LinkBuffer& linkBuffer)
{
    auto* start = static_cast<uint8_t*>(linkBuffer.entrypoint<JSEntryPtrTag>().untaggedPtr());
    Vector<StubCall> result;
    for (auto& pending : m_pending) {
        linkBuffer.link<JITThunkPtrTag>(pending.call, CodeLocationLabel<JITThunkPtrTag>(tagCodePtr<JITThunkPtrTag>(start)));
        auto* location = static_cast<uint8_t*>(linkBuffer.locationOfNearCall<JITThunkPtrTag>(pending.call).dataLocation());
#if CPU(X86_64)
        size_t sizeBeforeLocation = sizeOfNearCall;
#else
        size_t sizeBeforeLocation = pending.isTailCall ? 0 : sizeOfNearCall;
#endif
        result.append({ static_cast<uint32_t>(location - start - sizeBeforeLocation), pending.stub, pending.isTailCall, pending.thunk, pending.function, pending.callSite, pending.argumentMoves, 0, pending.firstOmittedGPR, pending.endOfOmittedGPRs });
    }
    return result;
}

size_t sizeOfOmittedArguments(unsigned firstGPR, unsigned endOfGPRs)
{
    size_t size = 0;
    for (unsigned i = firstGPR; i < endOfGPRs; ++i) {
#if CPU(X86_64)
        size += directArgumentGPR(i) >= X86Registers::r8 ? 6 : 5;
#else
        size += sizeof(uint32_t);
#endif
    }
    return size;
}

void writeOmittedArguments(uint8_t* base, size_t at, unsigned firstGPR, unsigned endOfGPRs)
{
    constexpr uint32_t undefined = JSValue::ValueUndefined;
    for (unsigned i = firstGPR; i < endOfGPRs; ++i) {
        uint32_t reg = static_cast<uint32_t>(directArgumentGPR(i));
#if CPU(X86_64)
        if (reg >= static_cast<uint32_t>(X86Registers::r8))
            base[at++] = 0x41;
        base[at++] = 0xb8 | (reg & 7);
        memcpy(base + at, &undefined, sizeof(undefined));
        at += sizeof(undefined);
#else
        uint32_t move = 0xd2800000 | undefined << 5 | reg;
        memcpy(base + at, &move, sizeof(move));
        at += sizeof(move);
#endif
    }
}

void writeVeneer(uint8_t* base, size_t veneer, size_t target)
{
#if CPU(ARM64)
    constexpr uint32_t scratch = ARM64Registers::ip0;
    int64_t pages = static_cast<int64_t>(target >> 12) - static_cast<int64_t>(veneer >> 12);
    RELEASE_ASSERT(pages >= -(1 << 20) && pages < (1 << 20));
    uint32_t instructions[3] = {
        0x90000000u | (static_cast<uint32_t>(pages) & 3) << 29 | (static_cast<uint32_t>(pages >> 2) & 0x7ffff) << 5 | scratch,
        0x91000000u | static_cast<uint32_t>(target & 0xfff) << 10 | scratch << 5 | scratch,
        0xd61f0000u | scratch << 5,
    };
    static_assert(sizeof(instructions) == sizeOfVeneer);
    memcpy(base + veneer, instructions, sizeof(instructions));
#elif CPU(X86_64)
    static_assert(sizeOfNearCall <= sizeOfVeneer);
    memset(base + veneer, 0xcc, sizeOfVeneer);
    retargetStubCall(base, veneer, target, true);
#else
    UNUSED_PARAM(base);
    UNUSED_PARAM(veneer);
    UNUSED_PARAM(target);
    RELEASE_ASSERT_NOT_REACHED();
#endif
}

void retargetStubCall(uint8_t* base, size_t instruction, size_t target, bool isTailCall)
{
#if CPU(ARM64)
    int64_t delta = (static_cast<int64_t>(target) - static_cast<int64_t>(instruction)) / 4;
    RELEASE_ASSERT(delta >= -(1 << 25) && delta < (1 << 25));
    uint32_t encoded = (isTailCall ? 0x14000000u : 0x94000000u) | (static_cast<uint32_t>(delta) & 0x03ffffffu);
    memcpy(base + instruction, &encoded, sizeof(encoded));
#elif CPU(X86_64)
    int64_t delta = static_cast<int64_t>(target) - static_cast<int64_t>(instruction + sizeOfNearCall);
    RELEASE_ASSERT(delta >= std::numeric_limits<int32_t>::min() && delta <= std::numeric_limits<int32_t>::max());
    int32_t encoded = static_cast<int32_t>(delta);
    base[instruction] = isTailCall ? 0xe9 : 0xe8;
    memcpy(base + instruction + 1, &encoded, sizeof(encoded));
#else
    UNUSED_PARAM(base);
    UNUSED_PARAM(instruction);
    UNUSED_PARAM(target);
    UNUSED_PARAM(isTailCall);
    RELEASE_ASSERT_NOT_REACHED();
#endif
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
