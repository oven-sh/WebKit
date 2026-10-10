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
#include "ThunkGenerators.h"
#include "VM.h"
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
#if CPU(ARM64)
struct LabelAddress {
    CCallHelpers::Label instruction;
    GPRReg reg;
    CCallHelpers::Label target;
};
static Vector<LabelAddress>* s_labelAddresses;
#endif
static CCallHelpers::Label& callTargetWithList() { static NeverDestroyed<CCallHelpers::Label> label; return label; }
static CCallHelpers::Label& callVarargsReturnAddress() { static NeverDestroyed<CCallHelpers::Label> label; return label; }
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

static void pushWithReturnAddress(CCallHelpers& jit, GPRReg reg)
{
#if CPU(ARM64)
    jit.pushPair(reg, CCallHelpers::linkRegister);
#else
    jit.push(reg);
#endif
}

static void popWithReturnAddress(CCallHelpers& jit, GPRReg reg)
{
#if CPU(ARM64)
    jit.popPair(reg, CCallHelpers::linkRegister);
#else
    jit.pop(reg);
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

#if CPU(X86_64)
void loadFunctionIndexAt(CCallHelpers& jit, GPRReg pc)
{
    static_assert(functionIndexGPR == T15);
    constexpr GPRReg offset = CCallHelpers::s_scratchRegister;
    ASSERT(pc != T15);
    jit.move(pc, offset);
    jit.sub64(Address(instanceGPR, Instance::offsetOfCode()), offset);
    jit.move(offset, T15);
    jit.urshift64(TrustedImm32(codeGranuleShift), T15);
    jit.lshift64(TrustedImm32(2), T15);
    jit.add64(Address(instanceGPR, Instance::offsetOfCodeGranules()), T15);
    jit.load32(Address(T15), T15);
    jit.push(pc);
    GPRReg start = pc;
    jit.move(T15, start);
    jit.lshift64(TrustedImm32(2), start);
    jit.add64(Address(instanceGPR, Instance::offsetOfSubsequentFunctionStarts()), start);
    CCallHelpers::Label next = jit.label();
    Jump found = jit.branch32(CCallHelpers::Above, Address(start), offset);
    jit.add32(TrustedImm32(1), T15);
    jit.add64(TrustedImm32(sizeof(uint32_t)), start);
    jit.jump().linkTo(next, &jit);
    found.link(&jit);
    jit.pop(pc);
}

static void loadCallerIndex(CCallHelpers& jit, unsigned bytesPushed = 0)
{
    constexpr GPRReg pc = T13;
    jit.push(pc);
    jit.loadPtr(Address(CCallHelpers::stackPointerRegister, bytesPushed + sizeof(CPURegister)), pc);
    loadFunctionIndexAt(jit, pc);
    jit.pop(pc);
}
#else
void loadFunctionIndexAt(CCallHelpers& jit, GPRReg pc)
{
    static_assert(functionIndexGPR == T15);
    ASSERT(noOverlap(pc, T14, T15));
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfCode()), T14);
    jit.subPtr(pc, T14, T14);
    jit.urshiftPtr(T14, TrustedImm32(codeGranuleShift), T15);
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

static void loadCallerIndex(CCallHelpers& jit, unsigned = 0)
{
    loadFunctionIndexAt(jit, CCallHelpers::linkRegister);
}
#endif

static void loadInfo(CCallHelpers& jit, GPRReg result)
{
    constexpr GPRReg instance = instanceGPR;
    ASSERT(result != T15);
    static_assert(sizeof(FunctionInfo) == 16);
    jit.lshiftPtr(T15, TrustedImm32(4), result);
#if CPU(X86_64)
    jit.add64(Address(instance, Instance::offsetOfInfos()), result);
#else
    jit.loadPtr(Address(instance, Instance::offsetOfInfos()), CCallHelpers::memoryTempRegister);
    jit.addPtr(CCallHelpers::memoryTempRegister, result);
#endif
}

static void loadDataForSlot(CCallHelpers& jit, GPRReg slot, GPRReg data)
{
    constexpr GPRReg instance = instanceGPR;
    ASSERT(noOverlap(slot, data, T15));
    jit.loadPtr(Address(instance, Instance::offsetOfSharedData()), data);
#if CPU(X86_64)
    static_assert(SharedData::size <= std::numeric_limits<int32_t>::max());
    jit.move(slot, CCallHelpers::s_scratchRegister);
    jit.sub64(data, CCallHelpers::s_scratchRegister);
    Jump isShared = jit.branch64(CCallHelpers::Below, CCallHelpers::s_scratchRegister, TrustedImm32(SharedData::size));
#else
    jit.subPtr(slot, data, CCallHelpers::memoryTempRegister);
    Jump isShared = jit.branchPtr(CCallHelpers::Below, CCallHelpers::memoryTempRegister, CCallHelpers::TrustedImmPtr(SharedData::size));
#endif
    jit.lshiftPtr(T15, TrustedImm32(1), data);
    jit.addPtr(instance, data);
    jit.load16(Address(data, Instance::offsetOfStates()), data);
    jit.loadPtr(CCallHelpers::BaseIndex(instance, data, CCallHelpers::TimesEight), data);
    isShared.link(&jit);
}

// `keepsAllRegisters`: for a stub whose caller counts on every register but the temporaries.
static void countSlotMiss(CCallHelpers& jit, GPRReg data, bool keepsAllRegisters = false)
{
    UNUSED_PARAM(keepsAllRegisters);
    constexpr GPRReg instance = instanceGPR;
    ASSERT(data == T10);
    jit.loadPtr(Address(instance, Instance::offsetOfSharedData()), T11);
    Jump hasOwnData = jit.branchPtr(CCallHelpers::NotEqual, data, T11);
    jit.move(T15, T11);
    jit.and32(TrustedImm32(Instance::numberOfMissCounters - 1), T11, T13);
    jit.addPtr(TrustedImm32(Instance::offsetOfMissCounters()), instance, T12);
    jit.getEffectiveAddress(CCallHelpers::BaseIndex(T12, T13, CCallHelpers::TimesTwo), T12);
    jit.load16(Address(T12), T13);
    jit.add32(TrustedImm32(1), T13);
    jit.store16(T13, Address(T12));
    static_assert(sizeof(FunctionInfo) == 16);
    jit.lshiftPtr(T11, TrustedImm32(4), T12);
#if CPU(X86_64)
    jit.add64(Address(instance, Instance::offsetOfInfos()), T12);
    jit.load16(Address(T12, FunctionInfo::offsetOfFlags()), T12);
    jit.urshift32(TrustedImm32(FunctionInfo::numberOfFlagBits), T12);
    jit.add32(TrustedImm32(Instance::missesBeyondItsSlots), T12);
    Jump notReachedYet = jit.branch32(CCallHelpers::Below, T13, T12);
    {
        constexpr GPRReg saved[] = { R0, A1, A2, A3, T10, T15 };
        preservingRegisters(jit, saved, [&] {
            jit.move(instance, A0);
            jit.move(T11, A1);
            jit.loadPtr(Address(instance, Instance::offsetOfRuntimeTable()), T13);
            jit.loadPtr(Address(T13, static_cast<unsigned>(Entry::operationAOTEnsureData) * sizeof(void*)), T13);
            jit.call(T13, OperationPtrTag);
        });
    }
#else
    jit.loadPtr(Address(instance, Instance::offsetOfInfos()), CCallHelpers::memoryTempRegister);
    jit.addPtr(CCallHelpers::memoryTempRegister, T12);
    jit.load16(Address(T12, FunctionInfo::offsetOfFlags()), T12);
    jit.urshift32(TrustedImm32(FunctionInfo::numberOfFlagBits), T12);
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

static void callAndCheckException(CCallHelpers& jit, GPRReg function, Returns returns, GPRReg result = GPRInfo::returnValueGPR)
{
    jit.emitFunctionPrologue();
#if CPU(X86_64)
    jit.push(eighthOperationArgumentGPR);
    jit.push(seventhOperationArgumentGPR);
#endif
    ASSERT(function != assertionScratchGPR);
    prepareCallOperation(jit, assertionScratchGPR);
    jit.call(function, OperationPtrTag);
    jit.emitFunctionEpilogue();
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
    loadInstance(jit, T9);
    jumpToEntry(jit, T9, Entry::HandleException);
}

enum class Supplies : uint8_t { Nothing, GlobalObject, Instance };
static void generateOperation(CCallHelpers& jit, Returns returns, Supplies supplies, GPRReg result = GPRInfo::returnValueGPR)
{
    loadInstance(jit, operationGPR);
    if (supplies == Supplies::GlobalObject)
        jit.loadPtr(Address(operationGPR, Instance::offsetOfGlobalObject()), A0);
    else if (supplies == Supplies::Instance)
        jit.move(operationGPR, A0);
    jit.loadPtr(Address(operationGPR, Instance::offsetOfRuntimeTable()), operationGPR);
    jit.loadPtr(CCallHelpers::BaseIndex(operationGPR, T9, CCallHelpers::TimesOne), operationGPR);
    callAndCheckException(jit, operationGPR, returns, result);
}

#if CPU(X86_64)
static void generateColdOperation(CCallHelpers& jit, bool isLeaf, bool returnsValue = false, bool checksException = true, bool suppliesInstance = true, bool returnsDouble = false, bool savesFPRs = true)
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
    jit.loadPtr(CCallHelpers::BaseIndex(operationGPR, T9, CCallHelpers::TimesOne), operationGPR);
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
static void generateThrowAtSite(CCallHelpers& jit, bool isLeaf, bool isFault = false)
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
    RELEASE_ASSERT(!isLeaf);
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
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T9);
    jit.loadPtr(Address(T9, static_cast<unsigned>(Entry::operationAOTTypedThrowAtSite) * sizeof(void*)), T9);
    prepareCallOperation(jit, T12);
    jit.call(T9, OperationPtrTag);
    jit.emitFunctionEpilogue();
    loadInstance(jit, T9);
    jumpToEntry(jit, T9, Entry::HandleException);
}
static void generateThrowAtSite(CCallHelpers& jit) { generateThrowAtSite(jit, false); }
static void generateThrowAtFault(CCallHelpers& jit) { generateThrowAtSite(jit, false, true); }
#if CPU(ARM64)
static void generateLeafThrowAtSite(CCallHelpers& jit) { generateThrowAtSite(jit, true); }
static void generateLeafThrowAtFault(CCallHelpers& jit) { generateThrowAtSite(jit, true, true); }
#else
static void generateLeafThrowAtSite(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafThrowAtFault(CCallHelpers& jit) { jit.breakpoint(); }
#endif

static void generateColdOperationVoid(CCallHelpers& jit) { generateColdOperation(jit, false); }
static void generateColdOperationValue(CCallHelpers& jit) { generateColdOperation(jit, false, true); }
static void generateColdPlainOperation(CCallHelpers& jit) { generateColdOperation(jit, false, true, false); }
static void generateColdOperationVoidWithoutFPRs(CCallHelpers& jit) { generateColdOperation(jit, false, false, true, true, false, false); }
static void generateColdOperationValueWithoutFPRs(CCallHelpers& jit) { generateColdOperation(jit, false, true, true, true, false, false); }
static void generateColdPlainOperationWithoutFPRs(CCallHelpers& jit) { generateColdOperation(jit, false, true, false, true, false, false); }
static void generateColdPlainOperationWithoutInstance(CCallHelpers& jit) { generateColdOperation(jit, false, true, false, false); }
static void generateColdPlainOperationDouble(CCallHelpers& jit) { generateColdOperation(jit, false, false, false, true, true); }
static void generateColdPlainOperationDoubleWithoutInstance(CCallHelpers& jit) { generateColdOperation(jit, false, false, false, false, true); }
static void generateColdOperationDouble(CCallHelpers& jit) { generateColdOperation(jit, false, false, true, true, true); }

static void generateOperationValue(CCallHelpers& jit) { generateOperation(jit, Returns::Value, Supplies::Nothing); }
static void generateOperationVoid(CCallHelpers& jit) { generateOperation(jit, Returns::Void, Supplies::Nothing); }
static void generateOperationDouble(CCallHelpers& jit) { generateOperation(jit, Returns::Double, Supplies::Nothing); }
static void generateOperationValueWithGlobalObject(CCallHelpers& jit) { generateOperation(jit, Returns::Value, Supplies::GlobalObject); }
static void generateOperationVoidWithGlobalObject(CCallHelpers& jit) { generateOperation(jit, Returns::Void, Supplies::GlobalObject); }
static void generateOperationDoubleWithGlobalObject(CCallHelpers& jit) { generateOperation(jit, Returns::Double, Supplies::GlobalObject); }
static void generateOperationValueWithInstance(CCallHelpers& jit) { generateOperation(jit, Returns::Value, Supplies::Instance); }
static void generateOperationVoidWithInstance(CCallHelpers& jit) { generateOperation(jit, Returns::Void, Supplies::Instance); }
static void generateOperationDoubleWithInstance(CCallHelpers& jit) { generateOperation(jit, Returns::Double, Supplies::Instance); }

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
static void generatePlainOperationWithGlobalObject(CCallHelpers& jit) { generatePlain(jit, Instance::offsetOfGlobalObject()); }
static void generatePlainOperationWithInstance(CCallHelpers& jit) { generatePlain(jit, std::nullopt, true); }
static void generatePlainOperationWithVM(CCallHelpers& jit) { generatePlain(jit, Instance::offsetOfVM()); }

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
    jit.move(CCallHelpers::TrustedImmPtr(SharedData::size), T12);
    Jump isNobodys = jit.branchPtr(CCallHelpers::Below, T11, T12);
    jit.store32(T10, Address(A3, OBJECT_OFFSETOF(Slot, offset)));
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

// T9: an array that is within the threshold of the barrier. T11: what has been stored into it, boxed. See Typed::State::rememberElement(). On ARM64 every register is kept but the temporaries.
static void generateRememberElement(CCallHelpers& jit, bool isLeaf)
{
    static_assert(!static_cast<unsigned>(CellState::PossiblyBlack));
    CCallHelpers::JumpList slow;
    CCallHelpers::JumpList done;
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfVM()), T12);
    slow.append(jit.branchTest8(CCallHelpers::NonZero, Address(T12, VM::offsetOfHeapMutatorShouldBeFenced())));
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
    jit.zeroExtend32ToWord(T9, T12);
    jit.load16(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesTwo), T12);
    // The first allocation, which takes the slow path, gets the row. A row has an allocator.
    slow.append(jit.branchTest32(CCallHelpers::Zero, T12));
#if CPU(ARM64)
    jit.subLeftShift64(T11, T12, TrustedImm32(5), T11);
#else
    jit.lshift64(TrustedImm32(5), T12);
    jit.subPtr(T12, T11);
#endif
    jit.loadPtr(Address(T11, OBJECT_OFFSETOF(Instance::TypedAllocation, allocator)), T12);
    jit.emitAllocateWithNonNullAllocator(cell, JITAllocator::variable(), T12, T13, slow, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
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
    jit.load64(Address(T11, OBJECT_OFFSETOF(Instance::TypedAllocation, header)), T13);
    jit.store64(T13, Address(cell, 0));
    if (init != TypedCellInit::Header) {
        jit.load64(Address(T11, OBJECT_OFFSETOF(Instance::TypedAllocation, words)), T13);
        jit.store64(T13, Address(cell, sizeof(JSCell)));
    }
    returnToTypedCaller(jit, isLeaf);

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
// T9: the index into a table of the instance whose entries are filled the first time they are asked for. Yields the entry.
static void generateLazyConstant(CCallHelpers& jit, bool isLeaf, ptrdiff_t table, Entry fill)
{
    constexpr GPRReg result = GPRInfo::returnValueGPR;
    Jump isUninitialized;
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
            jit.loadPtr(Address(instanceGPR, Instance::offsetOfCellCageBase()), T11);
#if CPU(ARM64)
            jit.addLeftShift64(T11, result, TrustedImm32(Typed::cell32Shift), result);
#else
            jit.lshift64(TrustedImm32(Typed::cell32Shift), result);
            jit.add64(T11, result);
#endif
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
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfCellCageBase()), T11);
#if CPU(ARM64)
        jit.addLeftShift64(T11, result, TrustedImm32(Typed::cell32Shift), result);
#else
        jit.lshift64(TrustedImm32(Typed::cell32Shift), result);
        jit.add64(T11, result);
#endif
        returnToTypedCaller(jit, isLeaf);
    } else {
        jit.zeroExtend32ToWord(T9, T12);
        jit.load64(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesEight), result);
        isUninitialized = jit.branchTest64(CCallHelpers::Zero, result);
        returnToTypedCaller(jit, isLeaf);
    }

    isUninitialized.link(&jit);
#if CPU(ARM64)
    jit.move(T9, T12);
    jit.move(TrustedImm32(static_cast<unsigned>(fill) * sizeof(void*)), T9);
    generateColdOperation(jit, isLeaf, true, false, true, false, true, std::nullopt, T12);
#else
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

// `fiber`: what the string in `string` points to; then its characters, if they have 8 bits and it is flat or a part of a flat one: what slice, substring and trim yield. `string` is kept.
static void loadCharacters8(CCallHelpers& jit, GPRReg string, GPRReg fiber, GPRReg scratch, CCallHelpers::JumpList& isLeftToOperation)
{
    constexpr int32_t isSubstringOf8Bits = JSRopeString::isSubstringInPointer | JSRopeString::is8BitInPointer;
    Jump isRope = jit.branchIfRopeStringImpl(fiber);
    isLeftToOperation.append(jit.branchTest32(CCallHelpers::Zero, Address(fiber, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.loadPtr(Address(fiber, StringImpl::dataOffset()), fiber);
    Jump isLoaded = jit.jump();
    isRope.link(&jit);
    jit.move(fiber, scratch);
    jit.and32(TrustedImm32(isSubstringOf8Bits), scratch);
    isLeftToOperation.append(jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(isSubstringOf8Bits)));
    loadBaseAndOffsetOfSubstring(jit, string, fiber, scratch);
    jit.loadPtr(Address(fiber, JSString::offsetOfValue()), fiber);
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
    isLeftToOperation.append(jit.branchTestPtr(CCallHelpers::NonZero, T12, TrustedImm32(JSString::isRopeInPointer)));
    // Pairs of offset and length.
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfTypedTexts()), T14);
    jit.load32(BaseIndex(T14, T9, CCallHelpers::TimesEight, sizeof(uint32_t)), result);
    jit.load32(BaseIndex(T14, T9, CCallHelpers::TimesEight), T14);
    isNot.append(jit.branch32(CCallHelpers::NotEqual, Address(T12, StringImpl::lengthMemoryOffset()), result));
    isLeftToOperation.append(jit.branchTest32(CCallHelpers::Zero, Address(T12, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.loadPtr(Address(T12, StringImpl::dataOffset()), T12);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfTypedData()), T13);
    jit.addPtr(T14, T13);

    compareBytesAndReturn(jit, isLeaf, isNot);

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

// The word of a closure of the typed compiler as an EntryWord: the address of its code, with the flags where the code that JITCode::m_entry
// goes through as well looks for them. `instance`: the one the closure is of. `closure` may be `result`.
static void loadEntryWordOfClosure(CCallHelpers& jit, GPRReg closure, GPRReg instance, GPRReg result, GPRReg scratch)
{
    constexpr unsigned distance = ClosureWord::constructsOrdinaryObjectBit - EntryWord::constructsOrdinaryObjectBit;
    static_assert(ClosureWord::numberOfParametersShift - EntryWord::numberOfParametersShift == distance && ClosureWord::isListBit - EntryWord::isListBit == distance && ClosureWord::isNotCallableBit - EntryWord::isNotCallableBit == distance);
    static_assert(ClosureWord::isNotCallableBit == 63 && EntryWord::constructsOrdinaryObjectBit == EntryWord::numberOfAddressBits);
    ASSERT(noOverlap(instance, result, scratch) && closure != scratch);
    static_assert(ClosureWord::numberOfOffsetBits == 32);
    jit.load64(Address(closure, Typed::Closure::offsetOfEntry()), result);
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
static void generateTypedClosureEntry(CCallHelpers& jit, bool isLeaf)
{
    constexpr GPRReg result = GPRInfo::returnValueGPR;
    static_assert(noOverlap(T9, T11, T12, result));
    CCallHelpers::JumpList isNoClosure;
    isNoClosure.append(jit.branchIfNotType(T9, JSFunctionType));
    jit.load32(Address(T9, JSCell::structureIDOffset()), T11);
    structureWithID(jit, T11);
    isNoClosure.append(jit.branchPtr(CCallHelpers::NotEqual, Address(T11, Structure::offsetOfAOTInstance()), instanceGPR));
    jit.load32(Address(instanceGPR, Instance::offsetOfTypedClosureStructureID(0)), T12);
    structureWithID(jit, T12);
    jit.loadPtr(Address(T12, Structure::classInfoOffset()), T12);
    isNoClosure.append(jit.branchPtr(CCallHelpers::NotEqual, Address(T11, Structure::classInfoOffset()), T12));
    jit.loadPtr(Address(T9, Typed::Closure::offsetOfEntry()), result);
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

// T9: a ClosureRequest. From the third argument register on: what the closure captures, by slot. Yields the closure, made as Lowering::lowerNewClosure() makes one in place. On ARM64 every register is kept but that and the temporaries.
static void generateNewTypedClosure(CCallHelpers& jit, bool isLeaf, unsigned numberOfCaptures)
{
    constexpr GPRReg cell = GPRInfo::returnValueGPR;
#if CPU(ARM64)
    static_assert(ClosureRequest::maxNumberOfCaptures == 4 && noOverlap(cell, T9, T11, T12, T13, A1, A2, A3, A4, A5));
#else
    static_assert(ClosureRequest::maxNumberOfCaptures == 2 && noOverlap(cell, T9, T11, T12, T13, A1, A2, A3));
#endif
    static_assert(sizeof(Instance::TypedAllocation) == 32);
    if (numberOfCaptures > ClosureRequest::maxNumberOfCaptures) {
        jit.breakpoint();
        return;
    }
    CCallHelpers::JumpList slow;
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
    returnToTypedCaller(jit, isLeaf);

    slow.link(&jit);
    uint64_t count = static_cast<uint64_t>(numberOfCaptures) << ClosureRequest::countShift;
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
            jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(count)), T11);
            jit.or64(T11, A1);
        }
        jit.move(instanceGPR, A0);
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T11);
        jit.loadPtr(Address(T11, static_cast<unsigned>(Entry::operationAOTTypedNewClosureSlow) * sizeof(void*)), T11);
        jit.call(T11, OperationPtrTag);
    });
    jit.ret();
#endif
}
static void generateNewTypedClosure0(CCallHelpers& jit) { generateNewTypedClosure(jit, false, 0); }
static void generateNewTypedClosure1(CCallHelpers& jit) { generateNewTypedClosure(jit, false, 1); }
static void generateNewTypedClosure2(CCallHelpers& jit) { generateNewTypedClosure(jit, false, 2); }
static void generateNewTypedClosure3(CCallHelpers& jit) { generateNewTypedClosure(jit, false, 3); }
static void generateNewTypedClosure4(CCallHelpers& jit) { generateNewTypedClosure(jit, false, 4); }
#if CPU(ARM64)
static void generateLeafNewTypedClosure0(CCallHelpers& jit) { generateNewTypedClosure(jit, true, 0); }
static void generateLeafNewTypedClosure1(CCallHelpers& jit) { generateNewTypedClosure(jit, true, 1); }
static void generateLeafNewTypedClosure2(CCallHelpers& jit) { generateNewTypedClosure(jit, true, 2); }
static void generateLeafNewTypedClosure3(CCallHelpers& jit) { generateNewTypedClosure(jit, true, 3); }
static void generateLeafNewTypedClosure4(CCallHelpers& jit) { generateNewTypedClosure(jit, true, 4); }
#else
static void generateLeafNewTypedClosure0(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafNewTypedClosure1(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafNewTypedClosure2(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafNewTypedClosure3(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafNewTypedClosure4(CCallHelpers& jit) { jit.breakpoint(); }
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
        needsContent.append(jit.branchIfRopeStringImpl(T9));
        needsContent.append(jit.branchIfRopeStringImpl(T10));
        isTrue.append(jit.branchPtr(CCallHelpers::Equal, T9, T10));
        jit.load32(Address(T9, StringImpl::lengthMemoryOffset()), T11);
        jit.load32(Address(T10, StringImpl::lengthMemoryOffset()), T12);
        isFalse.append(jit.branch32(CCallHelpers::NotEqual, T11, T12));
        jit.load32(Address(T9, StringImpl::flagsOffset()), T12);
        jit.and32(Address(T10, StringImpl::flagsOffset()), T12);
        isFalse.append(jit.branchTest32(CCallHelpers::NonZero, T12, TrustedImm32(StringImpl::flagIsAtom())));
        needsContent.append(jit.branchTest32(CCallHelpers::Zero, T12, TrustedImm32(StringImpl::flagIs8Bit())));
        // No way leads to the operation from here on: the operands are not needed.
        constexpr GPRReg left = T9;
        constexpr GPRReg right = T10;
        constexpr GPRReg remaining = T11;
        constexpr GPRReg leftCharacters = T12;
        constexpr GPRReg rightCharacters = R0;
        jit.loadPtr(Address(left, StringImpl::dataOffset()), left);
        jit.loadPtr(Address(right, StringImpl::dataOffset()), right);
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

static void generateLatin1Characters(CCallHelpers& jit)
{
    CCallHelpers::JumpList needsSlowLookup;
    jit.loadPtr(Address(R0, JSString::offsetOfValue()), T9);
    Jump isRope = jit.branchIfRopeStringImpl(T9);
    jit.load32(Address(T9, StringImpl::lengthMemoryOffset()), T10);
    jit.load32(Address(T9, StringImpl::flagsOffset()), T11);
    needsSlowLookup.append(jit.branchTest32(CCallHelpers::Zero, T11, TrustedImm32(StringImpl::flagIs8Bit())));
    jit.loadPtr(Address(T9, StringImpl::dataOffset()), R0);

    CCallHelpers::Label pack = jit.label();
    Jump fits = jit.branch32(CCallHelpers::BelowOrEqual, T10, TrustedImm32(0xffff));
    jit.move(TrustedImm32(0xffff), T10);
    fits.link(&jit);
    jit.lshift64(TrustedImm32(48), T10);
    jit.or64(T10, R0);
    jit.ret();

    isRope.link(&jit);
    jit.load32(Address(R0, JSRopeString::offsetOfLength()), T10);
    constexpr uintptr_t latin1SubstringBits = JSRopeString::isSubstringInPointer | JSRopeString::is8BitInPointer;
    jit.and64(TrustedImm32(latin1SubstringBits), T9, T11);
    needsSlowLookup.append(jit.branch64(CCallHelpers::NotEqual, T11, TrustedImm32(latin1SubstringBits)));
    jit.load64(Address(R0, JSRopeString::offsetOfFiber1()), T9);
    jit.load64(Address(R0, JSRopeString::offsetOfFiber2()), T11);
    jit.urshift64(TrustedImm32(32), T9);
    jit.and64(TrustedImm32(0xffff), T11, R0);
    jit.lshift64(TrustedImm32(32), R0);
    jit.or64(T9, R0);
    jit.urshift64(TrustedImm32(16), T11);
    jit.loadPtr(Address(R0, JSString::offsetOfValue()), R0);
    jit.load32(Address(R0, StringImpl::flagsOffset()), T9);
    needsSlowLookup.append(jit.branchTest32(CCallHelpers::Zero, T9, TrustedImm32(StringImpl::flagIs8Bit())));
    jit.loadPtr(Address(R0, StringImpl::dataOffset()), R0);
    jit.add64(T11, R0);
    jit.jump().linkTo(pack, &jit);

    needsSlowLookup.link(&jit);
    jit.move(TrustedImm32(0), R0);
    jit.jump().linkTo(pack, &jit);
}

static void generateIsStringEqualTo(CCallHelpers& jit)
{
    CCallHelpers::JumpList isTrue;
    CCallHelpers::JumpList isFalse;
    CCallHelpers::JumpList slow;
    isFalse.append(jit.branchIfNotCell(R0));
    jit.load8(Address(R0, JSCell::typeInfoTypeOffset()), T9);
    isFalse.append(jit.branch32(CCallHelpers::NotEqual, T9, TrustedImm32(StringType)));
    jit.loadPtr(Address(R0, JSString::offsetOfValue()), A2);
    jit.loadPtr(Address(A1, JSString::offsetOfValue()), A3);
    Jump isRope = jit.branchIfRopeStringImpl(A2);
    jit.load32(Address(A2, StringImpl::flagsOffset()), T9);
    jit.compare64(CCallHelpers::Equal, A2, A3, T11);
    jit.and32(TrustedImm32(StringImpl::flagIsAtom()), T9, A4);
    jit.or32(T11, A4);
    Jump isDifferentNonAtomString = jit.branchTest32(CCallHelpers::Zero, A4);
    jit.move(T11, R0);
    jit.ret();

    isDifferentNonAtomString.link(&jit);
    jit.load32(Address(A3, StringImpl::lengthMemoryOffset()), A4);
    jit.load32(Address(A2, StringImpl::lengthMemoryOffset()), T11);
    isFalse.append(jit.branch32(CCallHelpers::NotEqual, T11, A4));
    slow.append(jit.branchTest32(CCallHelpers::Zero, T9, TrustedImm32(StringImpl::flagIs8Bit())));
    jit.loadPtr(Address(A2, StringImpl::dataOffset()), A2);

    CCallHelpers::Label compare = jit.label();
    slow.append(jit.branchTest32(CCallHelpers::Zero, Address(A3, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.loadPtr(Address(A3, StringImpl::dataOffset()), A3);
    Jump fewerThanEight = jit.branch32(CCallHelpers::Below, A4, TrustedImm32(8));
    CCallHelpers::Label wordLoop = jit.label();
    Jump isLastWord = jit.branch32(CCallHelpers::BelowOrEqual, A4, TrustedImm32(8));
    jit.load64(Address(A2), T9);
    jit.load64(Address(A3), T11);
    isFalse.append(jit.branch64(CCallHelpers::NotEqual, T9, T11));
    jit.add64(TrustedImm32(8), A2);
    jit.add64(TrustedImm32(8), A3);
    jit.sub32(TrustedImm32(8), A4);
    jit.jump().linkTo(wordLoop, &jit);
    isLastWord.link(&jit);
    jit.add64(A4, A2);
    jit.add64(A4, A3);
    jit.load64(Address(A2, -8), T9);
    jit.load64(Address(A3, -8), T11);
    jit.compare64(CCallHelpers::Equal, T9, T11, R0);
    jit.ret();
    fewerThanEight.link(&jit);
    CCallHelpers::Label byteLoop = jit.label();
    isTrue.append(jit.branchTest32(CCallHelpers::Zero, A4));
    jit.load8(Address(A2), T9);
    jit.load8(Address(A3), T11);
    isFalse.append(jit.branch32(CCallHelpers::NotEqual, T9, T11));
    jit.add64(TrustedImm32(1), A2);
    jit.add64(TrustedImm32(1), A3);
    jit.sub32(TrustedImm32(1), A4);
    jit.jump().linkTo(byteLoop, &jit);

    isRope.link(&jit);
    jit.load32(Address(A3, StringImpl::lengthMemoryOffset()), A4);
    jit.load32(Address(R0, JSRopeString::offsetOfLength()), T9);
    isFalse.append(jit.branch32(CCallHelpers::NotEqual, T9, A4));
    constexpr uintptr_t latin1SubstringBits = JSRopeString::isSubstringInPointer | JSRopeString::is8BitInPointer;
    jit.and64(TrustedImm32(latin1SubstringBits), A2, T9);
    slow.append(jit.branch64(CCallHelpers::NotEqual, T9, TrustedImm32(latin1SubstringBits)));
    jit.load64(Address(R0, JSRopeString::offsetOfFiber1()), T9);
    jit.load64(Address(R0, JSRopeString::offsetOfFiber2()), T11);
    jit.urshift64(TrustedImm32(32), T9);
    jit.and64(TrustedImm32(0xffff), T11, A2);
    jit.lshift64(TrustedImm32(32), A2);
    jit.or64(T9, A2);
    jit.urshift64(TrustedImm32(16), T11);
    jit.loadPtr(Address(A2, JSString::offsetOfValue()), A2);
    slow.append(jit.branchTest32(CCallHelpers::Zero, Address(A2, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.loadPtr(Address(A2, StringImpl::dataOffset()), A2);
    jit.add64(T11, A2);
    jit.jump().linkTo(compare, &jit);

    isTrue.link(&jit);
    jit.move(TrustedImm32(1), R0);
    jit.ret();
    isFalse.link(&jit);
    jit.move(TrustedImm32(0), R0);
    jit.ret();

    slow.link(&jit);
    generateEqual(jit, Entry::operationAOTCompareStrictEq);
}
static void generateLooseEqual(CCallHelpers& jit) { generateEqual(jit, Entry::operationAOTCompareEq); }

static void compareWithProgramConstant(CCallHelpers& jit)
{
    pushWithReturnAddress(jit, R0);
    jit.move(instanceGPR, A0);
    jit.move(T9, A1);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T11);
    jit.loadPtr(Address(T11, static_cast<unsigned>(Entry::operationAOTProgramConstant) * sizeof(void*)), T11);
    jit.call(T11, OperationPtrTag);
    jit.move(GPRInfo::returnValueGPR, A1);
    popWithReturnAddress(jit, R0);
    generateEqual(jit, Entry::operationAOTCompareStrictEq);
}

static void generateIsStringEqualToConstant(CCallHelpers& jit)
{
    CCallHelpers::JumpList isTrue;
    CCallHelpers::JumpList isFalse;
    CCallHelpers::JumpList slow;
    isFalse.append(jit.branchIfNotCell(R0));
    jit.load8(Address(R0, JSCell::typeInfoTypeOffset()), T11);
    isFalse.append(jit.branch32(CCallHelpers::NotEqual, T11, TrustedImm32(StringType)));
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfStringConstantRecords()), A3);
    jit.load32(CCallHelpers::BaseIndex(A3, T9, CCallHelpers::TimesFour), A3);
    jit.add64(Address(instanceGPR, Instance::offsetOfProgramData()), A3);
    jit.load32(Address(A3), A4);
    jit.and32(TrustedImm32(0x7fffffff), A4);
    jit.loadPtr(Address(R0, JSString::offsetOfValue()), A2);
    Jump isRope = jit.branchIfRopeStringImpl(A2);
    jit.load32(Address(A2, StringImpl::lengthMemoryOffset()), T11);
    isFalse.append(jit.branch32(CCallHelpers::NotEqual, T11, A4));
    slow.append(jit.branchTest32(CCallHelpers::Zero, Address(A2, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.loadPtr(Address(A2, StringImpl::dataOffset()), A2);

    CCallHelpers::Label compare = jit.label();
    jit.add64(TrustedImm32(2 * sizeof(uint32_t)), A3);
    isTrue.append(jit.branch64(CCallHelpers::Equal, A2, A3));
    CCallHelpers::Label wordLoop = jit.label();
    Jump isLastWord = jit.branch32(CCallHelpers::BelowOrEqual, A4, TrustedImm32(8));
    jit.load64(Address(A2), A1);
    jit.load64(Address(A3), T11);
    isFalse.append(jit.branch64(CCallHelpers::NotEqual, A1, T11));
    jit.add64(TrustedImm32(8), A2);
    jit.add64(TrustedImm32(8), A3);
    jit.sub32(TrustedImm32(8), A4);
    jit.jump().linkTo(wordLoop, &jit);
    isLastWord.link(&jit);
    jit.add64(A4, A2);
    jit.add64(A4, A3);
    jit.load64(Address(A2, -8), A1);
    jit.load64(Address(A3, -8), T11);
    jit.compare64(CCallHelpers::Equal, A1, T11, R0);
    jit.ret();

    isRope.link(&jit);
    jit.load32(Address(R0, JSRopeString::offsetOfLength()), T11);
    isFalse.append(jit.branch32(CCallHelpers::NotEqual, T11, A4));
    constexpr uintptr_t latin1SubstringBits = JSRopeString::isSubstringInPointer | JSRopeString::is8BitInPointer;
    jit.and64(TrustedImm32(latin1SubstringBits), A2, T11);
    slow.append(jit.branch64(CCallHelpers::NotEqual, T11, TrustedImm32(latin1SubstringBits)));
    jit.load64(Address(R0, JSRopeString::offsetOfFiber1()), A1);
    jit.load64(Address(R0, JSRopeString::offsetOfFiber2()), T11);
    jit.urshift64(TrustedImm32(32), A1);
    jit.and64(TrustedImm32(0xffff), T11, A2);
    jit.lshift64(TrustedImm32(32), A2);
    jit.or64(A1, A2);
    jit.urshift64(TrustedImm32(16), T11);
    jit.loadPtr(Address(A2, JSString::offsetOfValue()), A2);
    slow.append(jit.branchTest32(CCallHelpers::Zero, Address(A2, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.loadPtr(Address(A2, StringImpl::dataOffset()), A2);
    jit.add64(T11, A2);
    jit.jump().linkTo(compare, &jit);

    isTrue.link(&jit);
    jit.move(TrustedImm32(1), R0);
    jit.ret();
    isFalse.link(&jit);
    jit.move(TrustedImm32(0), R0);
    jit.ret();

    slow.link(&jit);
    compareWithProgramConstant(jit);
}

static void generateIsStringEqualToShortLiteral(CCallHelpers& jit, unsigned chunkSize)
{
    CCallHelpers::JumpList isFalse;
    CCallHelpers::JumpList slow;
    isFalse.append(jit.branchIfNotCell(R0));
    jit.load8(Address(R0, JSCell::typeInfoTypeOffset()), T11);
    isFalse.append(jit.branch32(CCallHelpers::NotEqual, T11, TrustedImm32(StringType)));
    jit.and32(TrustedImm32(shortLiteralLengthMask), T9, A4);
    if (chunkSize == 16)
        jit.add32(TrustedImm32(1), A4);
    jit.loadPtr(Address(R0, JSString::offsetOfValue()), A2);
    Jump isRope = jit.branchIfRopeStringImpl(A2);
    jit.load32(Address(A2, StringImpl::lengthMemoryOffset()), T11);
    isFalse.append(jit.branch32(CCallHelpers::NotEqual, T11, A4));
    slow.append(jit.branchTest32(CCallHelpers::Zero, Address(A2, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.loadPtr(Address(A2, StringImpl::dataOffset()), A2);

    CCallHelpers::Label compare = jit.label();
    switch (chunkSize) {
    case 16:
        jit.load64(Address(A2), T11);
        jit.add64(A4, A2);
        jit.load64(Address(A2, -8), T12);
        jit.xor64(A1, T11);
        jit.xor64(A3, T12);
        jit.or64(T12, T11);
        jit.move(TrustedImm32(0), A1);
        break;
    case 1:
        jit.load8(Address(A2), T11);
        break;
    case 2:
        jit.load16Unaligned(Address(A2), T11);
        jit.add64(A4, A2);
        jit.load16Unaligned(Address(A2, -2), T12);
        jit.lshift64(TrustedImm32(16), T12);
        jit.or64(T12, T11);
        break;
    case 4:
        jit.load32(Address(A2), T11);
        jit.add64(A4, A2);
        jit.load32(Address(A2, -4), T12);
        jit.lshift64(TrustedImm32(32), T12);
        jit.or64(T12, T11);
        break;
    case 8:
        jit.load64(Address(A2), T11);
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    jit.compare64(CCallHelpers::Equal, T11, A1, R0);
    jit.ret();

    isRope.link(&jit);
    jit.load32(Address(R0, JSRopeString::offsetOfLength()), T11);
    isFalse.append(jit.branch32(CCallHelpers::NotEqual, T11, A4));
    constexpr uintptr_t latin1SubstringBits = JSRopeString::isSubstringInPointer | JSRopeString::is8BitInPointer;
    jit.and64(TrustedImm32(latin1SubstringBits), A2, T11);
    slow.append(jit.branch64(CCallHelpers::NotEqual, T11, TrustedImm32(latin1SubstringBits)));
    jit.load64(Address(R0, JSRopeString::offsetOfFiber1()), T12);
    jit.load64(Address(R0, JSRopeString::offsetOfFiber2()), T11);
    jit.urshift64(TrustedImm32(32), T12);
    jit.and64(TrustedImm32(0xffff), T11, A2);
    jit.lshift64(TrustedImm32(32), A2);
    jit.or64(T12, A2);
    jit.urshift64(TrustedImm32(16), T11);
    jit.loadPtr(Address(A2, JSString::offsetOfValue()), A2);
    slow.append(jit.branchTest32(CCallHelpers::Zero, Address(A2, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.loadPtr(Address(A2, StringImpl::dataOffset()), A2);
    jit.add64(T11, A2);
    jit.jump().linkTo(compare, &jit);

    isFalse.link(&jit);
    jit.move(TrustedImm32(0), R0);
    jit.ret();

    slow.link(&jit);
    jit.urshift32(TrustedImm32(shortLiteralLengthBits), T9);
    compareWithProgramConstant(jit);
}

static void generateIsStringEqualToLiteral1(CCallHelpers& jit) { generateIsStringEqualToShortLiteral(jit, 1); }
static void generateIsStringEqualToLiteral2To3(CCallHelpers& jit) { generateIsStringEqualToShortLiteral(jit, 2); }
static void generateIsStringEqualToLiteral4To7(CCallHelpers& jit) { generateIsStringEqualToShortLiteral(jit, 4); }
static void generateIsStringEqualToLiteral8(CCallHelpers& jit) { generateIsStringEqualToShortLiteral(jit, 8); }
static void generateIsStringEqualToLiteral9To16(CCallHelpers& jit) { generateIsStringEqualToShortLiteral(jit, 16); }

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

enum class Binary : uint8_t { Add, Sub, Mul, Div, BitAnd, BitOr, BitXor, LShift, RShift, URShift, Mod, Less, LessEq, Greater, GreaterEq };

static void generateBinary(CCallHelpers& jit, Binary kind, Entry operation, bool isBehindInt32Test = false)
{
    RELEASE_ASSERT(!isBehindInt32Test || kind != Binary::Mod);
    bool isComparison = kind >= Binary::Less;
    bool hasDoubleCase = isComparison || kind <= Binary::Div;
    bool isOnBits = kind >= Binary::BitAnd && kind <= Binary::URShift;
    constexpr FPRReg left = FPRInfo::fpRegT0;
    constexpr FPRReg right = FPRInfo::fpRegT1;
    CCallHelpers::JumpList notBothInt32;
    CCallHelpers::JumpList slow;

    if (!isBehindInt32Test) {
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
            jit.branchConvertDoubleToInt32(left, T11, notInt32, right);
            boxInt32AndReturn();
            notInt32.link(&jit);
            jit.moveDoubleTo64(left, R0);
            jit.sub64(numberTag, R0);
            jit.ret();
        }
    }

    if (isOnBits) {
        slow.append(jit.branchTest64(CCallHelpers::Zero, R0, numberTag));
        slow.append(jit.branchTest64(CCallHelpers::Zero, A1, numberTag));
        auto toInt32 = [&](GPRReg value, GPRReg result) {
            jit.move(value, result);
            Jump isInt32 = jit.branch64(CCallHelpers::AboveOrEqual, value, numberTag);
            jit.add64(numberTag, value, T11);
            jit.move64ToDouble(T11, left);
            slow.append(jit.branchTruncateDoubleToInt32(left, result));
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
static void generateDivBehindInt32Test(CCallHelpers& jit) { generateBinary(jit, Binary::Div, Entry::operationAOTValueDiv, true); }
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

static void getFromMegamorphicCache(CCallHelpers&, GPRReg uid, CCallHelpers::JumpList& notFound, GPRReg slotToCountAttemptIn = InvalidGPRReg);

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
        jit.branchConvertDoubleToInt32(number, T11, isNoInt32, FPRInfo::fpRegT1);
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

static void generatePutByValDirect(CCallHelpers& jit)
{
    CCallHelpers::JumpList slow;
    slow.append(jit.branch64(CCallHelpers::Below, A1, numberTag));
    jit.load8(Address(R0, JSCell::indexingTypeAndMiscOffset()), T11);
    jit.and32(TrustedImm32(IsArray | IndexingShapeMask | CopyOnWrite), T11);
    Jump isContiguous = jit.branch32(CCallHelpers::Equal, T11, TrustedImm32(ArrayWithContiguous));
    slow.append(jit.branch32(CCallHelpers::NotEqual, T11, TrustedImm32(ArrayWithInt32)));
    slow.append(jit.branch64(CCallHelpers::Below, A2, numberTag));
    isContiguous.link(&jit);
    jit.loadPtr(Address(R0, JSObject::butterflyOffset()), T11);
    jit.zeroExtend32ToWord(A1, T12);
    slow.append(jit.branch32(CCallHelpers::AboveOrEqual, T12, Address(T11, Butterfly::offsetOfVectorLength())));
    jit.store64(A2, CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesEight));
    Jump isWithinLength = jit.branch32(CCallHelpers::Below, T12, Address(T11, Butterfly::offsetOfPublicLength()));
    jit.add32(TrustedImm32(1), T12, T13);
    jit.store32(T13, Address(T11, Butterfly::offsetOfPublicLength()));
    isWithinLength.link(&jit);

    returnAfterStoring(jit, A2);

    slow.link(&jit);
    loadInstance(jit, T11);
    jit.move(A3, A4);
    jit.move(A2, A3);
    jit.move(A1, A2);
    jit.move(R0, A1);
    jit.move(T11, A0);
    jit.loadPtr(Address(T11, Instance::offsetOfRuntimeTable()), T11);
    jit.loadPtr(Address(T11, static_cast<unsigned>(Entry::operationAOTPutByValDirect) * sizeof(void*)), T11);
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
    return jit.branchTest64(CCallHelpers::NonZero, firstSlotWord, CCallHelpers::TrustedImm64(static_cast<int64_t>(flag) << 32));
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
    jit.subPtr(TrustedImm32(Data::offsetOfSlots()), result);
    jit.urshiftPtr(TrustedImm32(4), result);
}

static void prepareMissAtSite(CCallHelpers& jit, Entry operation, unsigned numberOfOperands, unsigned bytesPushed = 0)
{
    RELEASE_ASSERT(numberOfOperands >= 1 && numberOfOperands <= 3);
    constexpr GPRReg arguments[] = { operationArgumentGPR(0), operationArgumentGPR(1), operationArgumentGPR(2), operationArgumentGPR(3), operationArgumentGPR(4), operationArgumentGPR(5), operationArgumentGPR(6) };
    GPRReg site = arguments[numberOfOperands];
    loadCallerIndex(jit, bytesPushed);
    loadDataForSlot(jit, site, T10);
    slotSite(jit, T10, site, T13);
    loadInfo(jit, T11);
    loadSites(jit, T11, T11);
    static_assert(sizeof(Site) == 4);
    jit.load32(CCallHelpers::BaseIndex(T11, T13, CCallHelpers::TimesFour), T12);
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

static void missAtSite(CCallHelpers& jit, Entry operation, unsigned numberOfOperands, Returns returns)
{
    prepareMissAtSite(jit, operation, numberOfOperands);
    callAndCheckException(jit, T9, returns);
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
    jit.moveConditionally64(CCallHelpers::LessThan, word, TrustedImm32(0), storage, object, storage);
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

static void checkStackBeforeCallingAccessor(CCallHelpers& jit, GPRReg vm, GPRReg stackPointer)
{
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfVM()), vm);
    jit.move(CCallHelpers::stackPointerRegister, stackPointer);
    Jump fits = jit.branchPtr(CCallHelpers::BelowOrEqual, Address(vm, VM::offsetOfSoftStackLimit()), stackPointer);
    s_callsBetweenStubs->append({ jit.nearTailCall(), Stub::ThrowStackOverflow });
    fits.link(&jit);
}

static void callGetter(CCallHelpers& jit, CCallHelpers::JumpList& cannot)
{
    jit.add32(TrustedImm32(1), Address(instanceGPR, Instance::offsetOfEffectEpoch()));
    {
        jit.loadPtr(Address(T12, GetterSetter::offsetOfGetterShortcut()), T13);
        Jump hasNoShortcut = jit.branchTestPtr(CCallHelpers::Zero, T13);
        Jump isCode = jit.branchTestPtr(CCallHelpers::NonZero, T13, TrustedImm32(GetterSetter::getterShortcutIsCode));
        jit.load64(Address(T13), T13);
        Jump isEmpty = jit.branchTest64(CCallHelpers::Zero, T13);
        jit.move(T13, R0);
        jit.ret();

        isCode.link(&jit);
        static_assert(calleeGPR == T10);
        jit.loadPtr(Address(T12, GetterSetter::offsetOfGetter()), T11);
        jit.load32(Address(T11, JSCell::structureIDOffset()), T10);
        structureWithID(jit, T10);
        Jump belongsToThisInstance = jit.branchPtr(CCallHelpers::Equal, Address(T10, Structure::offsetOfAOTInstance()), instanceGPR);
        Jump belongsToAnotherInstance = jit.branchTestPtr(CCallHelpers::NonZero, Address(T10, Structure::offsetOfAOTInstance()));
        jit.loadPtr(Address(T10, Structure::realmOffset()), T10);
        Jump belongsToAnotherRealm = jit.branchPtr(CCallHelpers::NotEqual, Address(instanceGPR, Instance::offsetOfGlobalObject()), T10);
        belongsToThisInstance.link(&jit);
        checkStackBeforeCallingAccessor(jit, T10, T12);
        jit.move(R0, thisGPR);
        jit.move(T11, calleeGPR);
        jit.subPtr(TrustedImm32(GetterSetter::getterShortcutIsCode), T13);
        jit.farJump(T13, JSEntryPtrTag);

        hasNoShortcut.link(&jit);
        isEmpty.link(&jit);
        belongsToAnotherInstance.link(&jit);
        belongsToAnotherRealm.link(&jit);
    }
    jit.loadPtr(Address(T12, GetterSetter::offsetOfGetter()), T12);
    {
        jit.load32(Address(T12, JSCell::structureIDOffset()), T13);
        Jump isNotBound = jit.branch32(CCallHelpers::NotEqual, T13, Address(instanceGPR, Instance::offsetOfBoundFunctionStructureID()));
        Jump hasOtherArguments = jit.branch32(CCallHelpers::NotEqual, Address(T12, JSBoundFunction::offsetOfBoundArgsLength()), TrustedImm32(1));
        jit.load32(Address(T12, JSBoundFunction::offsetOfCachedStructureOfBoundThis()), T13);
        Jump hasNothingCached = jit.branchTest32(CCallHelpers::Zero, T13);
        jit.loadPtr(Address(T12, JSBoundFunction::offsetOfBoundThis()), T11);
        cannot.append(jit.branch32(CCallHelpers::NotEqual, T13, Address(T11, JSCell::structureIDOffset())));
        jit.load32(Address(T12, JSBoundFunction::offsetOfCachedLocationInBoundThis()), T13);
        jit.signExtend32ToPtr(T13, T13);
        jit.loadPtr(Address(T11, JSObject::butterflyOffset()), T12);
        jit.moveConditionally64(CCallHelpers::LessThan, T13, TrustedImm32(0), T12, T11, T12);
        Jump isAccessor = jit.branchTest32(CCallHelpers::NonZero, T13, TrustedImm32(1));
        jit.load64(CCallHelpers::BaseIndex(T12, T13, CCallHelpers::TimesFour), R0);
        jit.ret();
        isAccessor.link(&jit);
        jit.load64(CCallHelpers::BaseIndex(T12, T13, CCallHelpers::TimesFour, -static_cast<int32_t>(sizeof(EncodedJSValue) / 2)), T12);
        jit.loadPtr(Address(T12, GetterSetter::offsetOfGetterShortcut()), T12);
        cannot.append(jit.branchTestPtr(CCallHelpers::Zero, T12));
        cannot.append(jit.branchTestPtr(CCallHelpers::NonZero, T12, TrustedImm32(GetterSetter::getterShortcutIsCode)));
        jit.load64(Address(T12), T12);
        cannot.append(jit.branchTest64(CCallHelpers::Zero, T12));
        jit.move(T12, R0);
        jit.ret();
        isNotBound.link(&jit);
        hasOtherArguments.link(&jit);
        hasNothingCached.link(&jit);
    }
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
    cannot.append(jit.branchIfNotType(T12, JSFunctionType));
    checkStackBeforeCallingAccessor(jit, T11, T13);
    jit.move(R0, thisGPR);
    jit.move(T12, calleeGPR);
    jit.move(TrustedImm32(0), countGPR);
    jit.jump().linkTo(stubLabels()[static_cast<unsigned>(Stub::Call)], &jit);
}

static void getFromMegamorphicCache(CCallHelpers& jit, GPRReg uid, CCallHelpers::JumpList& notFound, GPRReg slotToCountAttemptIn)
{
#if CPU(X86_64)
    constexpr GPRReg cache = T10;
    constexpr GPRReg result = T9;
#else
    constexpr GPRReg cache = GPRInfo::argumentGPR7;
    constexpr GPRReg result = GPRInfo::argumentGPR6;
#endif
    ASSERT(uid != cache && uid != result && uid != R0 && uid != A1);
    ASSERT(slotToCountAttemptIn == InvalidGPRReg || noOverlap(slotToCountAttemptIn, cache, result, uid, T11, T12, T13));
    auto countAttempt = [&] {
        if (slotToCountAttemptIn == InvalidGPRReg)
            return;
        Jump isNone = jit.branchTestPtr(CCallHelpers::Zero, slotToCountAttemptIn);
        jit.load32(Address(slotToCountAttemptIn, OBJECT_OFFSETOF(Slot, offset)), T11);
        jit.add32(TrustedImm32(1u << Slot::attemptsShift), T11);
        jit.store32(T11, Address(slotToCountAttemptIn, OBJECT_OFFSETOF(Slot, offset)));
        isNone.link(&jit);
    };
    loadInstance(jit, cache);
    jit.loadPtr(Address(cache, Instance::offsetOfRuntimeTable()), cache);
    jit.loadPtr(Address(cache, static_cast<unsigned>(Entry::MegamorphicCache) * sizeof(void*)), cache);
    notFound.append(jit.branchTestPtr(CCallHelpers::Zero, cache));
    CCallHelpers::JumpList notValue = jit.loadMegamorphicProperty(CCallHelpers::MegamorphicCacheLocation(cache), R0, uid, nullptr, result, T11, T12, T13);
    countAttempt();
    jit.move(result, R0);
    jit.ret();
    notValue.link(&jit);
    notFound.append(jit.loadMegamorphicGetterSetter(CCallHelpers::MegamorphicCacheLocation(cache), R0, uid, nullptr, result, T11, T12, T13));
    countAttempt();
    jit.move(result, T12);
    callGetter(jit, notFound);
}

static void generateGetByIdWith(CCallHelpers&, Entry);
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
    isNotNamed.append(jit.branchTest64(CCallHelpers::Zero, T13));
    loadDirectLocation(jit, A1, T11, T14);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfFieldIDAtDirectLocationBase()), T9);
    jit.add64(T12, T9);
    jit.load16(CCallHelpers::BaseIndex(T9, T14, CCallHelpers::TimesTwo), T9);
    isNotNamed.append(jit.branch32(CCallHelpers::NotEqual, T9, T13));
    jit.load64(CCallHelpers::BaseIndex(base, T14, CCallHelpers::TimesEight), R0);
    jit.ret();
}

static void generateGetByIdFrom(CCallHelpers& jit, Entry operation, GPRReg base)
{
    ASSERT(base != R0 && base != A1 && base != T11 && base != T12);
    Jump isNotCell = jit.branchIfNotCell(base);
    jit.load64(slotWord(A1, 0), T11);
    jit.load32(Address(base, JSCell::structureIDOffset()), T12);
    Jump hasDifferentStructure = jit.branch32(CCallHelpers::NotEqual, T11, T12);
    Jump isIndirect = jit.branchTest64(CCallHelpers::NonZero, T11, CCallHelpers::TrustedImm64(static_cast<int64_t>(Slot::isIndirect) << 32));
    extractBits(jit, T11, 32, Slot::directLocationBits, T11);
    jit.load64(CCallHelpers::BaseIndex(base, T11, CCallHelpers::TimesEight), R0);
    jit.ret();

    auto& continuations = getByIdContinuations(operation);
    isIndirect.link(&jit);
    jit.move(base, R0);
    jit.jump().linkTo(continuations.isIndirect, &jit);
    hasDifferentStructure.link(&jit);
    if (operation == Entry::operationAOTGetById) {
        CCallHelpers::JumpList isAlsoIndirect;
        CCallHelpers::JumpList isNotNamed;
        readByName(jit, base, isAlsoIndirect, isNotNamed);
        isAlsoIndirect.link(&jit);
        jit.move(base, R0);
        jit.jump().linkTo(continuations.hasDifferentStructureAndIsIndirect, &jit);
        isNotNamed.link(&jit);
        jit.move(base, R0);
        jit.jump().linkTo(continuations.isNotNamed, &jit);
    } else {
        jit.move(base, R0);
        jit.jump().linkTo(continuations.hasDifferentStructure, &jit);
    }
    isNotCell.link(&jit);
    jit.move(base, R0);
    jit.jump().linkTo(continuations.miss, &jit);
}

struct ReadSlotContinuations {
    CCallHelpers::Label absentLabel;
    CCallHelpers::Label miss;
};
static ReadSlotContinuations& readSlotContinuations(bool allowsUndefined, unsigned slot)
{
    static NeverDestroyed<std::array<std::array<ReadSlotContinuations, Structure::numberOfSlotsWithFieldIDs>, 2>> continuations;
    return continuations.get()[allowsUndefined][slot];
}

static void generateReadSlot(CCallHelpers& jit, unsigned slot, bool allowsUndefined, GPRReg base = R0)
{
    static_assert(Structure::numberOfSlotsWithFieldIDs == 16);
    ASSERT(base != A1 && base != T11 && base != T12 && base != T13);
    Jump isNotCell = jit.branchIfNotCell(base);
    jit.load32(Address(base, JSCell::structureIDOffset()), T13);
    structureWithID(jit, T13);
    jit.load16(Address(T13, Structure::offsetOfFieldIDInSlot() + slot * sizeof(uint16_t)), T11);
    Jump absentLabel = jit.branch32(CCallHelpers::NotEqual, T11, A1);
    jit.load64(Address(base, JSObject::offsetOfInlineStorage() + slot * sizeof(EncodedJSValue)), R0);
    jit.ret();

    auto& continuations = readSlotContinuations(allowsUndefined, slot);
    if (base != R0) {
        absentLabel.link(&jit);
        jit.move(base, R0);
        jit.jump().linkTo(continuations.absentLabel, &jit);
        isNotCell.link(&jit);
        jit.move(base, R0);
        jit.jump().linkTo(continuations.miss, &jit);
        return;
    }

    absentLabel.link(&jit);
    continuations.absentLabel = jit.label();
    CCallHelpers::JumpList miss;
    if (allowsUndefined) {
        miss.append(jit.branch32(CCallHelpers::Equal, T11, TrustedImm32(Structure::ambiguousFieldID)));
        jit.load16(Address(T13, Structure::offsetOfTypedLayoutID()), T11);
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T12);
        jit.loadPtr(Address(T12, (static_cast<unsigned>(Entry::FieldLayoutIDsInSlot0) + slot) * sizeof(void*)), T12);
        jit.load16(CCallHelpers::BaseIndex(T12, A1, CCallHelpers::TimesTwo), T12);
        miss.append(jit.branch32(CCallHelpers::NotEqual, T11, T12));
        jit.move(CCallHelpers::TrustedImm64(JSValue::encode(jsUndefined())), R0);
        jit.ret();
    }
    miss.link(&jit);
    isNotCell.link(&jit);
    continuations.miss = jit.label();
    jit.or32(TrustedImm32(slot << 16 | allowsUndefined << 24), A1);
    callBinaryOperation(jit, Entry::operationAOTReadField);
}

static std::optional<std::pair<unsigned, bool>> slotReadBy(Stub stub)
{
    unsigned number = static_cast<unsigned>(stub);
    if (number >= static_cast<unsigned>(Stub::ReadSlot0) && number < static_cast<unsigned>(Stub::ReadSlot0) + Structure::numberOfSlotsWithFieldIDs)
        return std::pair { number - static_cast<unsigned>(Stub::ReadSlot0), false };
    if (number >= static_cast<unsigned>(Stub::ReadSlotOrUndefined0) && number < static_cast<unsigned>(Stub::ReadSlotOrUndefined0) + Structure::numberOfSlotsWithFieldIDs)
        return std::pair { number - static_cast<unsigned>(Stub::ReadSlotOrUndefined0), true };
    return std::nullopt;
}
#define AOT_READ_SLOT(n) \
static void generateReadSlot##n(CCallHelpers& jit) { generateReadSlot(jit, n, false); } \
static void generateReadSlotOrUndefined##n(CCallHelpers& jit) { generateReadSlot(jit, n, true); }
AOT_READ_SLOT(0)
AOT_READ_SLOT(1)
AOT_READ_SLOT(2)
AOT_READ_SLOT(3)
AOT_READ_SLOT(4)
AOT_READ_SLOT(5)
AOT_READ_SLOT(6)
AOT_READ_SLOT(7)
AOT_READ_SLOT(8)
AOT_READ_SLOT(9)
AOT_READ_SLOT(10)
AOT_READ_SLOT(11)
AOT_READ_SLOT(12)
AOT_READ_SLOT(13)
AOT_READ_SLOT(14)
AOT_READ_SLOT(15)
#undef AOT_READ_SLOT

static void generateGetById(CCallHelpers& jit) { generateGetByIdWith(jit, Entry::operationAOTGetById); }
static void generateGetByIdWellKnown(CCallHelpers& jit) { generateGetByIdWith(jit, Entry::operationAOTGetByIdWellKnown); }

// R0: an object. T12: the number of a name. See Instance::NamedField. Where the table has the pair, `access` is what the entry says and control goes on; T10, T11 and T13 are used. T12 is kept where it has not.
static void findNamedField(CCallHelpers& jit, ptrdiff_t offsetOfTable, GPRReg access, CCallHelpers::JumpList& isNotThere)
{
    ASSERT(access == T12 || access == T13);
    jit.load8(Address(R0, JSCell::typeInfoTypeOffset()), T11);
    isNotThere.append(jit.branch32(CCallHelpers::NotEqual, T11, TrustedImm32(ObjectType)));
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
        jit.zeroExtend32ToWord(value, value);
        jit.add64(numberTag, value, R0);
        jit.ret();
    };
    CCallHelpers::JumpList isAbsent;
    CCallHelpers::JumpList isUndefined;
    CCallHelpers::JumpList isNull;
    static_assert(Slot::fieldIsInExtension == 1ull << (Slot::fieldReadShift + 4));
    Jump isInExtension = hasBit(4);
    jit.zeroExtend32ToWord(T12, address);
    jit.addPtr(R0, address);
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
        jit.lshift64(TrustedImm32(Typed::cell32Shift), value);
        jit.add64(Address(instanceGPR, Instance::offsetOfCellCageBase()), value);
        returnValue();
        isNoCell.link(&jit);
        static_assert(!Typed::absentCell32);
        isAbsent.append(jit.branchTest32(CCallHelpers::Zero, value));
        returnValue();

        isAmongUpperTwo.link(&jit);
        jit.load32(Address(address), value);
        Jump isInt32 = hasBit(0);
        static_assert(static_cast<unsigned>(FieldRead::Int32OrElsewhere) == 2 && static_cast<unsigned>(FieldRead::Int32) == 3);
        Jump isNoNumberInPlace = jit.branch32(CCallHelpers::LessThan, value, TrustedImm32(Typed::leastInt32InPlace));
        isInt32.link(&jit);
        returnInt32();
        isNoNumberInPlace.link(&jit);
        isAbsent.append(jit.branch32(CCallHelpers::Equal, value, TrustedImm32(Typed::absentInt32)));
        isUndefined.append(jit.branch32(CCallHelpers::Equal, value, TrustedImm32(Typed::undefinedInt32)));
        isNull.append(jit.branch32(CCallHelpers::Equal, value, TrustedImm32(Typed::nullInt32)));
        rare.append(jit.jump());
    }
    isAmongUpperFour.link(&jit);
    Jump isAmongLastTwo = hasBit(1);
    {
        Jump isBoolean = hasBit(0);
        static_assert(static_cast<unsigned>(FieldRead::Double) == 4);
        jit.loadDouble(Address(address), FPRInfo::fpRegT0);
        Jump isNaN = jit.branchIfNaN(FPRInfo::fpRegT0);
        CCallHelpers::JumpList isNoInt32;
        jit.branchConvertDoubleToInt32(FPRInfo::fpRegT0, value, isNoInt32, FPRInfo::fpRegT1);
        returnInt32();
        isNoInt32.link(&jit);
        jit.moveDoubleTo64(FPRInfo::fpRegT0, value);
        jit.sub64(numberTag, value);
        returnValue();
        isNaN.link(&jit);
        jit.moveDoubleTo64(FPRInfo::fpRegT0, value);
        isAbsent.append(jit.branch64(CCallHelpers::Equal, value, CCallHelpers::TrustedImm64(static_cast<int64_t>(Typed::absentDouble))));
        isUndefined.append(jit.branch64(CCallHelpers::Equal, value, CCallHelpers::TrustedImm64(static_cast<int64_t>(Typed::undefinedDouble))));
        isNull.append(jit.branch64(CCallHelpers::Equal, value, CCallHelpers::TrustedImm64(static_cast<int64_t>(Typed::nullDouble))));
        rare.append(jit.jump());

        static_assert(static_cast<unsigned>(FieldRead::Boolean) == 5);
        isBoolean.link(&jit);
        jit.load8(Address(address), value);
        jit.or32(TrustedImm32(JSValue::ValueFalse), value);
        returnValue();
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
    jit.loadPtr(Address(R0, Typed::offsetOfExtension), address);
    jit.load32(Address(address, Typed::offsetOfExtensionSize), value);
    // The low half of the word is the offset. Both are multiples of eight.
    isAbsent.append(jit.branch32(CCallHelpers::BelowOrEqual, value, T12));
    jit.zeroExtend32ToWord(T12, value);
    jit.addPtr(value, address);
    jit.load64(Address(address), value);
    isAbsent.append(jit.branchTest64(CCallHelpers::Zero, value));
    returnValue();

    isAbsent.link(&jit);
    rare.append(jit.branchTest64(CCallHelpers::Zero, T12, CCallHelpers::TrustedImm64(static_cast<int64_t>(Slot::absentFieldIsUndefined))));
    isUndefined.link(&jit);
    jit.move(CCallHelpers::TrustedImm64(JSValue::ValueUndefined), R0);
    jit.ret();
    isNull.link(&jit);
    jit.move(CCallHelpers::TrustedImm64(JSValue::ValueNull), R0);
    jit.ret();

    isNarrow.link(&jit);
    jit.zeroExtend32ToWord(T12, address);
    jit.addPtr(R0, address);
    Jump isUint32 = hasBit(2);
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
    jit.load32(Address(address), value);
    Jump isNoInt32 = jit.branch32(CCallHelpers::LessThan, value, TrustedImm32(0));
    returnInt32();
    isNoInt32.link(&jit);
    jit.convertInt64ToDouble(value, FPRInfo::fpRegT0);
    jit.moveDoubleTo64(FPRInfo::fpRegT0, value);
    jit.sub64(numberTag, value);
    returnValue();
}

static void generateGetByIdWith(CCallHelpers& jit, Entry operation)
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
    getByIdContinuations(operation).isIndirect = jit.label();
    jit.loadPtr(slotWord(A1, 1), T12);
    jit.moveConditionallyTest64(CCallHelpers::NonZero, T12, T12, T12, R0, T12);
    Jump isGetter = branchIfSlotHas<Slot::isGetter>(jit, T11);
    locateCachedProperty(jit, T12, T11, T13);
    jit.load64(CCallHelpers::BaseIndex(T13, T11, CCallHelpers::TimesEight), R0);
    jit.ret();

    isGetter.link(&jit);
    {
        Jump isField = branchIfSlotHas<Slot::pointerIsNotCell>(jit, T11);
        locateCachedProperty(jit, T12, T11, T13);
        jit.load64(CCallHelpers::BaseIndex(T13, T11, CCallHelpers::TimesEight), T12);
        callGetter(jit, miss);
        isField.link(&jit);
        readCompiledField(jit, miss, &isNoField);
    }

    hasDifferentStructure.link(&jit);
    getByIdContinuations(operation).hasDifferentStructure = jit.label();
#if CPU(X86_64)
    jit.load32(Address(R0, JSCell::structureIDOffset()), T12);
#endif
    CCallHelpers::JumpList missAndUpdateCache;
    if (operation == Entry::operationAOTGetById) {
        constexpr GPRReg several = T13;
#if CPU(X86_64)
        constexpr GPRReg siteOwnSlot = A2;
        constexpr GPRReg table = T10;
        constexpr GPRReg nameID = T9;
        constexpr GPRReg nameIDThere = T12;
#else
        constexpr GPRReg siteOwnSlot = T14;
        constexpr GPRReg table = GPRInfo::argumentGPR6;
        constexpr GPRReg nameID = A5;
        constexpr GPRReg nameIDThere = A4;
#endif
        {
            CCallHelpers::JumpList isAlsoIndirect;
            CCallHelpers::JumpList isNotNamed;
            readByName(jit, R0, isAlsoIndirect, isNotNamed);
            isNotNamed.link(&jit);
            getByIdContinuations(operation).isNotNamed = jit.label();
            missAndUpdateCache.append(jit.branchTest64(CCallHelpers::NonZero, T11, CCallHelpers::TrustedImm64(static_cast<int64_t>(~(static_cast<uint64_t>(Slot::attemptsMask) << 32)))));
            miss.append(jit.jump());
            isAlsoIndirect.link(&jit);
            getByIdContinuations(operation).hasDifferentStructureAndIsIndirect = jit.label();
        }
        missAndUpdateCache.append(jit.branchTest32(CCallHelpers::NonZero, T11));
        // No structure, and indirect: of such slots only one that stands for several has this flag.
        static_assert(Slot::polymorphicFlags == (Slot::isIndirect | Slot::pointerIsNotCell));
        miss.append(jit.branchTest64(CCallHelpers::Zero, T11, CCallHelpers::TrustedImm64(static_cast<int64_t>(Slot::pointerIsNotCell) << 32)));
        jit.loadPtr(slotWord(A1, 1), several);
        jit.load32(Address(R0, JSCell::structureIDOffset()), T12);
        static_assert(!OBJECT_OFFSETOF(Slot, structureID));

        // The one slot that the structure can have there: PolymorphicSlots::indexFor().
        static_assert(sizeof(Slot) == 16 && PolymorphicSlots::structureShift >= 4);
        jit.move(A1, siteOwnSlot);
        jit.and32(TrustedImm32((PolymorphicSlots::numberOfSlots - 1) << PolymorphicSlots::structureShift), T12, T11);
        jit.urshift32(TrustedImm32(PolymorphicSlots::structureShift - 4), T11);
        jit.addPtr(several, T11, A1);
        jit.addPtr(TrustedImm32(PolymorphicSlots::offsetOfSlots()), A1);
        jit.load64(slotWord(A1, 0), T11);
        Jump entryMatch = jit.branch32(CCallHelpers::Equal, T11, T12);
        jit.move(siteOwnSlot, A1);

        // The property by its name's number, which the structure records for its first inline slots: for any number of structures.
        constexpr GPRReg byName = T11;
        constexpr GPRReg inlineSlot = A2;
        auto loadInlineNameSlot = [&](unsigned i, GPRReg result) {
            extractBits(jit, byName, PolymorphicSlots::inlineNameSlotsShift + i * 8, 8, result);
        };
        jit.load64(Address(several, PolymorphicSlots::offsetOfByName()), byName);
        jit.and32(TrustedImm32(0xffff), byName, nameID);
        Jump hasNoInlineNameSlots = jit.branch32(CCallHelpers::Equal, nameID, TrustedImm32(Structure::firstReservedPropertyNameID));
        jit.zeroExtend32ToWord(T12, table);
        structureWithID(jit, table);
        jit.addPtr(TrustedImm32(Structure::offsetOfFieldIDInSlot()), table);
        loadInlineNameSlot(0, inlineSlot);
        jit.load16(CCallHelpers::BaseIndex(table, inlineSlot, CCallHelpers::TimesTwo), nameIDThere);
        Jump isInFirstInlineNameSlot = jit.branch32(CCallHelpers::Equal, nameIDThere, nameID);
        for (unsigned i = 1; i < PolymorphicSlots::numberOfInlineSlotsByName; ++i) {
            loadInlineNameSlot(i, A3);
            jit.load16(CCallHelpers::BaseIndex(table, A3, CCallHelpers::TimesTwo), nameIDThere);
            jit.moveConditionally32(CCallHelpers::Equal, nameIDThere, nameID, A3, inlineSlot, inlineSlot);
        }
        jit.load16(CCallHelpers::BaseIndex(table, inlineSlot, CCallHelpers::TimesTwo), nameIDThere);
        Jump isNotInInlineNameSlots = jit.branch32(CCallHelpers::NotEqual, nameIDThere, nameID);
        isInFirstInlineNameSlot.link(&jit);
        jit.load64(CCallHelpers::BaseIndex(R0, inlineSlot, CCallHelpers::TimesEight, JSObject::offsetOfInlineStorage()), R0);
        jit.ret();
        isNotInInlineNameSlots.link(&jit);
        jit.urshift64(byName, TrustedImm32(PolymorphicSlots::nameTableFillValueShift), nameID);
        for (unsigned i = 0; i < PolymorphicSlots::numberOfInlineSlotsByName; ++i) {
            loadInlineNameSlot(i, A3);
            jit.load16(CCallHelpers::BaseIndex(table, A3, CCallHelpers::TimesTwo), nameIDThere);
            missAndUpdateCache.append(jit.branch32(CCallHelpers::Equal, nameIDThere, nameID));
        }
        hasNoInlineNameSlots.link(&jit);
        jit.load32(Address(several, PolymorphicSlots::offsetOfMisses()), T12);
        jit.add32(TrustedImm32(1), T12);
        jit.store32(T12, Address(several, PolymorphicSlots::offsetOfMisses()));
        Jump hasHadItsFirstLessons = jit.branchTest32(CCallHelpers::Zero, Address(several, PolymorphicSlots::offsetOfRemainingBulkLearnAttempts()));
        missAndUpdateCache.append(jit.branchTest32(CCallHelpers::Zero, T12, TrustedImm32(PolymorphicSlots::missesPerEarlyLesson - 1)));
        Jump isNoLesson = jit.jump();
        hasHadItsFirstLessons.link(&jit);
        missAndUpdateCache.append(jit.branchTest32(CCallHelpers::Zero, T12, TrustedImm32(PolymorphicSlots::missesPerLearningAttempt - 1)));
        isNoLesson.link(&jit);
        missAndUpdateCache.append(jit.branchIfNotObject(R0));
        CCallHelpers::JumpList isNoKnownField;
        jit.load32(Address(several, PolymorphicSlots::offsetOfIdentifierIndex()), T12);
        findNamedField(jit, Instance::offsetOfNamedFieldReads(), T12, isNoKnownField);
        Jump isKnownField = jit.jump();
        isNoKnownField.link(&jit);
        jit.loadPtr(slotWord(A1, 1), several);
        jit.loadPtr(Address(several, PolymorphicSlots::offsetOfName()), A2);
        getFromMegamorphicCache(jit, A2, missAndUpdateCache);

        entryMatch.link(&jit);
        Jump isIndirectLocation = branchIfSlotHas<Slot::isIndirect>(jit, T11);
        loadDirectLocation(jit, A1, T11, T11);
        jit.load64(CCallHelpers::BaseIndex(R0, T11, CCallHelpers::TimesEight), R0);
        jit.ret();
        isIndirectLocation.link(&jit);
        jit.loadPtr(slotWord(A1, 1), T12);
        jit.moveConditionallyTest64(CCallHelpers::NonZero, T12, T12, T12, R0, T12);
        Jump isGetterThere = jit.branchTest64(CCallHelpers::NonZero, T11, CCallHelpers::TrustedImm64(static_cast<int64_t>(Slot::isGetter) << 32));
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
        readCompiledField(jit, cannotCallGetter);
        cannotCallGetter.link(&jit);
        jit.move(siteOwnSlot, A1);
        missAndUpdateCache.append(jit.jump());
    }

    miss.link(&jit);
    getByIdContinuations(operation).miss = jit.label();
    if (operation == Entry::operationAOTGetById) {
        loadCallerIndex(jit);
        loadDataForSlot(jit, A1, T10);
        countSlotMiss(jit, T10);

        CCallHelpers::JumpList notFound;
        notFound.append(jit.branchIfNotCell(R0));
        notFound.append(jit.branchIfNotObject(R0));
        jit.load32(Address(A1, OBJECT_OFFSETOF(Slot, offset)), T11);
        jit.and32(TrustedImm32(Slot::attemptsMask), T11);
        constexpr GPRReg untriedSlot = T14;
        jit.move(CCallHelpers::TrustedImmPtr(nullptr), untriedSlot);
        // T10: the Data, since the miss was counted.
        Jump hasGivenUp = jit.branch32(CCallHelpers::Equal, T11, TrustedImm32(Slot::attemptsMask));
        Jump isTaken = jit.branchTest32(CCallHelpers::NonZero, Address(A1, OBJECT_OFFSETOF(Slot, structureID)));
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfSharedData()), T12);
        Jump isUnowned = jit.branchPtr(CCallHelpers::Equal, T10, T12);
        notFound.append(jit.branchTest32(CCallHelpers::NonZero, T11));
        jit.move(A1, untriedSlot);
        Jump isUntried = jit.jump();
        hasGivenUp.link(&jit);
        isTaken.link(&jit);
        isUnowned.link(&jit);
        isUntried.link(&jit);
        slotSite(jit, T10, A1, T13);
        loadInfo(jit, T10);
        loadSites(jit, T10, T11);
        jit.load32(CCallHelpers::BaseIndex(T11, T13, CCallHelpers::TimesFour), T12);
        jit.and32(TrustedImm32((1u << Site::identifierBits) - 1), T12);
        {
            CCallHelpers::JumpList isNoKnownField;
            // Only for a slot that cannot learn. One that can goes on to the operation, which fills it.
            isNoKnownField.append(jit.branchTestPtr(CCallHelpers::NonZero, untriedSlot));
            findNamedField(jit, Instance::offsetOfNamedFieldReads(), T12, isNoKnownField);
            readCompiledField(jit, notFound);
            isNoKnownField.link(&jit);
        }
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfProgramIdentifiers()), T11);
        jit.loadPtr(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesEight), A2);
        notFound.append(jit.branchTestPtr(CCallHelpers::Zero, A2));
        getFromMegamorphicCache(jit, A2, notFound, untriedSlot);
        notFound.link(&jit);
    }
    isNoField.link(&jit);
    missAtSite(jit, operation, 1, Returns::Value);
    if (!missAndUpdateCache.empty()) {
        missAndUpdateCache.link(&jit);
        missAtSite(jit, Entry::RawGetById, 1, Returns::Value);
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
    jit.zeroExtend32ToWord(T13, address);
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
        unlessAccepts(FieldWrite::acceptsObject);
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

    isInt32OrElsewhere.link(&jit);
    {
        static_assert(Typed::elsewhereInt32 < Typed::absentInt32 && Typed::absentInt32 < Typed::undefinedInt32 && Typed::absentInt32 < Typed::nullInt32);
        if (!created)
            miss.append(jit.branch32(CCallHelpers::LessThanOrEqual, Address(address), TrustedImm32(Typed::absentInt32)));
        miss.append(jit.branchIfNotInt32(A1));
        unlessAccepts(FieldWrite::acceptsInt32);
        miss.append(jit.branch32(CCallHelpers::LessThan, A1, TrustedImm32(Typed::leastInt32InPlace)));
        jit.store32(A1, Address(address));
        didStore(false);
    }

    isCell32.link(&jit);
    {
        static_assert(!Typed::absentCell32 && Typed::nullCell32 == JSValue::ValueNull && Typed::undefinedCell32 == JSValue::ValueUndefined);
        if (!created)
            miss.append(jit.branchTest32(CCallHelpers::Zero, Address(address)));
        Jump isNoCell = jit.branchIfNotCell(A1);
        jit.load8(Address(A1, JSCell::typeInfoTypeOffset()), scratch);
        Jump isString = jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(StringType));
        miss.append(jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(SymbolType)));
        unlessAccepts(FieldWrite::acceptsSymbol);
        Jump isAcceptedSymbol = jit.jump();
        isString.link(&jit);
        unlessAccepts(FieldWrite::acceptsString);
        isAcceptedSymbol.link(&jit);
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
    miss.append(jit.branchIfNotCell(R0));
    jit.load64(slotWord(A2, 0), T11);
    miss.append(branchIfStructureDiffers(jit, R0, T11));
    jit.load64(slotWord(A2, 1), T13);
#if CPU(X86_64)
    Jump slotHasFieldType = jit.branchTest32(CCallHelpers::NonZero, slotWord(A2, 1).withOffset(sizeof(uint32_t)));
#else
    jit.urshift64(T13, TrustedImm32(32), T14);
    Jump slotHasFieldType = jit.branchTest32(CCallHelpers::NonZero, T14);
#endif
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

    {
        slotHasFieldType.link(&jit);
#if CPU(X86_64)
        jit.load32(slotWord(A2, 1).withOffset(sizeof(uint32_t)), T14);
#endif
        {
            Jump isNoCompiledField = jit.branchTest64(CCallHelpers::Zero, T11, CCallHelpers::TrustedImm64(static_cast<int64_t>(Slot::pointerIsNotCell) << 32));
            static_assert(!(Slot::FieldWrite::isFieldWrite & Slot::isStoreTransition >> 32));
            Jump makesTransition = jit.branchTest32(CCallHelpers::NonZero, T14, TrustedImm32(static_cast<int32_t>(Slot::isStoreTransition >> 32)));
            jit.and32(TrustedImm32(Slot::FieldWrite::isFieldWrite | Slot::FieldWrite::isNoField), T14, T12);
            isNoField.append(jit.branch32(CCallHelpers::Equal, T12, TrustedImm32(Slot::FieldWrite::isFieldWrite | Slot::FieldWrite::isNoField)));
            storeCompiledField(jit, miss);
            makesTransition.link(&jit);
            storeByTransition(jit, miss);
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
        {
            Jump acceptsAnyString = jit.branchTest32(CCallHelpers::Zero, T14, TrustedImm32(TypedLayoutTable::stringsAreAtoms));
            jit.load8(Address(A1, JSCell::typeInfoFlagsOffset()), T12);
            Jump isAtom = jit.branchTest32(CCallHelpers::NonZero, T12, TrustedImm32(TypeInfoPerCellBit));
            jit.loadPtr(Address(A1, JSString::offsetOfValue()), T12);
            Jump stringIsRope = jit.branchIfRopeStringImpl(T12);
            jit.load32(Address(T12, StringImpl::lengthMemoryOffset()), T12);
            Jump lengthReady = jit.jump();
            stringIsRope.link(&jit);
            jit.load32(Address(A1, JSRopeString::offsetOfLength()), T12);
            lengthReady.link(&jit);
            miss.append(jit.branch32(CCallHelpers::BelowOrEqual, T12, TrustedImm32(TypedLayoutTable::maxAtomizedStringLength)));
            isAtom.link(&jit);
            acceptsAnyString.link(&jit);
        }
        ifAccepts(SoundTypeString);
        isNotString.link(&jit);
        Jump isNotArray = jit.branch32(CCallHelpers::NotEqual, T12, TrustedImm32(ArrayType));
        ifAccepts(SoundTypeArray);
        isNotArray.link(&jit);
        Jump isNotFunction = jit.branch32(CCallHelpers::NotEqual, T12, TrustedImm32(JSFunctionType));
        ifAccepts(SoundTypeFunction);
        isNotFunction.link(&jit);
        miss.append(jit.branchTest32(CCallHelpers::NonZero, T14, TrustedImm32((TypedLayoutTable::stringsAreAtoms - 1) & ~SoundTypeAll)));
        Jump isFinalObject = jit.branch32(CCallHelpers::Equal, T12, TrustedImm32(FinalObjectType));
        miss.append(jit.branch32(CCallHelpers::Below, T12, TrustedImm32(ObjectType)));
        miss.append(jit.branch32(CCallHelpers::Equal, T12, TrustedImm32(InternalFunctionType)));
        miss.append(jit.branch32(CCallHelpers::Equal, T12, TrustedImm32(DerivedArrayType)));
        jit.load8(Address(A1, JSCell::typeInfoFlagsOffset()), T10);
        miss.append(jit.branchTest32(CCallHelpers::NonZero, T10, TrustedImm32(OverridesGetCallData)));
        jit.urshift32(T14, TrustedImm32(16), T10);
        miss.append(jit.branchTest32(CCallHelpers::NonZero, T10));
        ifAccepts(SoundTypeOtherObject);
        isFinalObject.link(&jit);
        miss.append(jit.branchTest32(CCallHelpers::Zero, T14, TrustedImm32(SoundTypeOtherObject)));
        jit.urshift32(T14, TrustedImm32(16), T12);
        jit.branchTest32(CCallHelpers::Zero, T12).linkTo(isHeld, &jit);
        jit.load32(Address(A1, JSCell::structureIDOffset()), T10);
        structureWithID(jit, T10);
        jit.load16(Address(T10, Structure::offsetOfTypedLayoutID()), T10);
        jit.branch32(CCallHelpers::Equal, T10, T12).linkTo(isHeld, &jit);
        miss.append(jit.jump());
    }

    miss.link(&jit);
    loadCallerIndex(jit);
    loadDataForSlot(jit, A2, T10);
    countSlotMiss(jit, T10);
    {
        // What has a put of its own is served by none of the tables below.
        Jump isNoCell = jit.branchIfNotCell(R0);
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
        isNoCell.link(&jit);
    }
    {
        CCallHelpers::JumpList notFound;
#if CPU(X86_64)
        constexpr GPRReg cache = T10;
#else
        constexpr GPRReg cache = GPRInfo::argumentGPR7;
#endif
        notFound.append(jit.branchIfNotCell(R0));
        notFound.append(jit.branchIfNotObject(R0));
        loadDataForSlot(jit, A2, T10);
        slotSite(jit, T10, A2, T13);
        loadInfo(jit, T10);
        loadSites(jit, T10, T11);
        jit.load32(CCallHelpers::BaseIndex(T11, T13, CCallHelpers::TimesFour), T12);
        Jump isNotDirect = jit.branchTest32(CCallHelpers::Zero, T12, TrustedImm32(1u << Site::identifierBits));
        jit.load32(Address(R0, JSCell::structureIDOffset()), T11);
        structureWithID(jit, T11);
        notFound.append(jit.branchTest32(CCallHelpers::NonZero, Address(T11, Structure::bitFieldOffset()), TrustedImm32(Structure::s_hasNonEnumerablePropertiesBits | Structure::s_hasNonConfigurablePropertiesBits)));
        isNotDirect.link(&jit);
        jit.and32(TrustedImm32((1u << Site::identifierBits) - 1), T12);
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfProgramIdentifiers()), T11);
        jit.loadPtr(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesEight), A3);
        notFound.append(jit.branchTestPtr(CCallHelpers::Zero, A3));
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), cache);
        jit.loadPtr(Address(cache, static_cast<unsigned>(Entry::MegamorphicCache) * sizeof(void*)), cache);
        notFound.append(jit.branchTestPtr(CCallHelpers::Zero, cache));
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
        notFound.append(jit.branchIfNotType(T12, JSFunctionType));
        checkStackBeforeCallingAccessor(jit, T11, T13);
        jit.add32(TrustedImm32(1), Address(instanceGPR, Instance::offsetOfEffectEpoch()));
        jit.move(T12, calleeGPR);
        jit.move(R0, thisGPR);
        jit.move(A1, argumentGPR(0));
        jit.move(TrustedImm32(1), countGPR);
        jit.jump().linkTo(stubLabels()[static_cast<unsigned>(Stub::Call)], &jit);

        notFound.link(&jit);
    }
    isNoField.link(&jit);
    missAtSite(jit, Entry::operationAOTPutById, 2, Returns::Void);
}

static void checkPrivateNameCache(CCallHelpers& jit, GPRReg slot, CCallHelpers::JumpList& miss)
{
    miss.append(jit.branchIfNotCell(R0));
    jit.load64(slotWord(slot, 0), T11);
    miss.append(branchIfStructureDiffers(jit, R0, T11));
    miss.append(jit.branchPtr(CCallHelpers::NotEqual, slotWord(slot, 1), A1));
}

static void generateGetPrivateName(CCallHelpers& jit)
{
    CCallHelpers::JumpList miss;
    checkPrivateNameCache(jit, A2, miss);
    locateCachedProperty(jit, R0, T11, T12);
    jit.load64(CCallHelpers::BaseIndex(T12, T11, CCallHelpers::TimesEight), R0);
    jit.ret();

    miss.link(&jit);
    missAtSite(jit, Entry::operationAOTGetPrivateName, 2, Returns::Value);
}

static void generateCheckPrivateBrand(CCallHelpers& jit)
{
    CCallHelpers::JumpList miss;
    checkPrivateNameCache(jit, A2, miss);
    jit.ret();

    miss.link(&jit);
    missAtSite(jit, Entry::operationAOTCheckPrivateBrand, 2, Returns::Void);
}

static void generatePutPrivateName(CCallHelpers& jit)
{
    CCallHelpers::JumpList miss;
    checkPrivateNameCache(jit, A3, miss);
    locateCachedProperty(jit, R0, T11, T12);
    jit.store64(A2, CCallHelpers::BaseIndex(T12, T11, CCallHelpers::TimesEight));

    returnAfterStoring(jit, A2);

    miss.link(&jit);
    missAtSite(jit, Entry::operationAOTPutPrivateName, 3, Returns::Void);
}

// R0 and A1 are cells. See Instance::keyedReads. Where it goes on, T11 is the first word of the entry for the two, which says where the property is.
static void findInKeyedTable(CCallHelpers& jit, ptrdiff_t offsetOfTable, unsigned numberOfEntries, GPRReg entry, CCallHelpers::JumpList& miss, CCallHelpers::JumpList& isNotForTable, unsigned sizeOfEntry = sizeof(Slot))
{
    RELEASE_ASSERT(hasOneBitSet(sizeOfEntry));
    jit.load32(Address(R0, JSCell::structureIDOffset()), T12);
    jit.urshift32(T12, TrustedImm32(Instance::keyedStructureShift), T11);
    jit.urshift64(A1, TrustedImm32(Instance::keyedKeyShift), entry);
    jit.add32(T11, entry);
    jit.and32(TrustedImm32(numberOfEntries - 1), entry);
    jit.lshift32(TrustedImm32(getLSBSet(sizeOfEntry)), entry);
    jit.loadPtr(Address(instanceGPR, offsetOfTable), T11);
    jit.addPtr(T11, entry);
    jit.load64(slotWord(entry, 0), T11);
    miss.append(jit.branch32(CCallHelpers::NotEqual, T11, T12));
    miss.append(jit.branchPtr(CCallHelpers::NotEqual, slotWord(entry, 1), A1));
    isNotForTable.append(branchIfSlotHas<Slot::isGetter>(jit, T11));
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
    Jump isInherited = jit.branchTest64(CCallHelpers::NonZero, T11, CCallHelpers::TrustedImm64(static_cast<int64_t>(static_cast<uint64_t>(Slot::pointerIsCell) << 32)));
    static_assert(Instance::keyIsCompiledField == (Slot::isGetter | Slot::isIndirect));
    generally.append(jit.branchTest64(CCallHelpers::Zero, T11, CCallHelpers::TrustedImm64(static_cast<int64_t>(static_cast<uint64_t>(Slot::isIndirect) << 32))));
    jit.loadPtr(Address(A2, OBJECT_OFFSETOF(Instance::KeyedRead, holder)), T12);
    readCompiledField(jit, generally);

    isInherited.link(&jit);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T12);
    jit.loadPtr(Address(T12, static_cast<unsigned>(Entry::MegamorphicCache) * sizeof(void*)), T12);
    jit.load16(Address(T12, MegamorphicCache::offsetOfEpoch()), T12);
    miss.append(jit.branch32(CCallHelpers::NotEqual, T12, Address(A2, OBJECT_OFFSETOF(Instance::KeyedRead, epoch))));
    jit.loadPtr(Address(A2, OBJECT_OFFSETOF(Instance::KeyedRead, holder)), T13);
    Jump isNowhere = jit.branchTestPtr(CCallHelpers::Zero, T13);
    locateCachedProperty(jit, T13, T11, T12);
    jit.load64(CCallHelpers::BaseIndex(T12, T11, CCallHelpers::TimesEight), R0);
    jit.ret();
    isNowhere.link(&jit);
    jit.move(CCallHelpers::TrustedImm64(JSValue::ValueUndefined), R0);
    jit.ret();

    generally.linkTo(stubLabels()[static_cast<unsigned>(Stub::GetByVal)], &jit);

    miss.link(&jit);
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
    findInKeyedTable(jit, Instance::offsetOfKeyedHas(), Instance::numberOfKeyedHas, A2, miss, miss);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T12);
    jit.loadPtr(Address(T12, static_cast<unsigned>(Entry::MegamorphicCache) * sizeof(void*)), T12);
    jit.load16(Address(T12, MegamorphicCache::offsetOfEpoch()), T12);
    jit.urshift64(TrustedImm32(32), T11);
    jit.and32(TrustedImm32((1u << Instance::keyedHasAnswerShift) - 1), T11, T13);
    miss.append(jit.branch32(CCallHelpers::NotEqual, T12, T13));
    jit.urshift32(TrustedImm32(Instance::keyedHasAnswerShift), T11);
    jit.and32(TrustedImm32(1), T11, R0);
    jit.ret();

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
    miss.append(jit.branchTest64(CCallHelpers::Zero, T11, CCallHelpers::TrustedImm64(static_cast<int64_t>(Slot::pointerIsNotCell) << 32)));
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

static void generatePutByValCached(CCallHelpers& jit)
{
    constexpr uint32_t isStrict = 1;
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
    callOperationOnThree(jit, Entry::operationAOTPutByValAndCache);
}

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
static void generateDeleteByValCached(CCallHelpers& jit)
{
    CCallHelpers::JumpList miss;
    miss.append(jit.branchIfNotCell(A1));
    checkPrivateNameCache(jit, A2, miss);
    loadStructureIDAfterTransition(jit, A2, T13, miss);
    locateCachedProperty(jit, R0, T11, T12);
    jit.store64(TrustedImm32(0), CCallHelpers::BaseIndex(T12, T11, CCallHelpers::TimesEight));
    jit.store32(T13, Address(R0, JSCell::structureIDOffset()));
    returnAfterStoring(jit, R0);

    miss.link(&jit);
    missAtSite(jit, Entry::operationAOTDeleteByValAndCache, 2, Returns::Void);
}

static void generateDefinePrivateName(CCallHelpers& jit)
{
    CCallHelpers::JumpList miss;
    checkPrivateNameCache(jit, A3, miss);
    loadStructureIDAfterTransition(jit, A3, T13, miss);
    locateCachedProperty(jit, R0, T11, T12);
    jit.store64(A2, CCallHelpers::BaseIndex(T12, T11, CCallHelpers::TimesEight));
    jit.store32(T13, Address(R0, JSCell::structureIDOffset()));
    returnAfterStoring(jit, R0);

    miss.link(&jit);
    missAtSite(jit, Entry::operationAOTPutPrivateName, 3, Returns::Void);
}

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

static void generateSetPrivateBrand(CCallHelpers& jit)
{
    CCallHelpers::JumpList miss;
    checkPrivateNameCache(jit, A2, miss);
    loadStructureIDAfterTransition(jit, A2, T13, miss);
    jit.store32(T13, Address(R0, JSCell::structureIDOffset()));
    returnAfterStoring(jit, R0);

    miss.link(&jit);
    missAtSite(jit, Entry::operationAOTSetPrivateBrand, 2, Returns::Void);
}

static Jump walkScopeChainIfResolvedByDepth(CCallHelpers& jit)
{
    Jump miss = jit.branchTest32(CCallHelpers::Zero, T11, TrustedImm32(Slot::resolvesByDepth));
    jit.and32(TrustedImm32(~Slot::resolvesByDepth), T11);
    Jump there = jit.branchTest32(CCallHelpers::Zero, T11);
    auto loop = jit.label();
    jit.loadPtr(Address(R0, JSScope::offsetOfNext()), R0);
    jit.branchSub32(CCallHelpers::NonZero, TrustedImm32(1), T11).linkTo(loop, &jit);
    there.link(&jit);
    return miss;
}

static void generateResolveScope(CCallHelpers& jit)
{
    jit.load32(slotWord(A1, 0).withOffset(sizeof(uint32_t)), T11);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfGlobalObject()), T12);
    jit.load32(Address(T12, JSGlobalObject::offsetOfGlobalLexicalBindingEpoch()), T12);
    jit.add32(TrustedImm32(1), T12);
    Jump entryMismatch = jit.branch32(CCallHelpers::NotEqual, T11, T12);
    jit.loadPtr(slotWord(A1, 1), R0);
    jit.ret();

    entryMismatch.link(&jit);
    Jump miss = walkScopeChainIfResolvedByDepth(jit);
    jit.ret();

    miss.link(&jit);
    missAtSite(jit, Entry::operationAOTResolveScope, 1, Returns::Value);
}

static void getFromScopeIfCached(CCallHelpers& jit, unsigned firstWord, CCallHelpers::JumpList& miss)
{
    jit.load64(slotWord(A1, firstWord), T11);
    miss.append(branchIfStructureDiffers(jit, R0, T11));
    jit.loadPtr(slotWord(A1, firstWord + 1), T12);
    Jump noAddress = jit.branchTestPtr(CCallHelpers::Zero, T12);
    static_assert(Slot::pointerIsCell == 1u << 31);
    Jump isAddress = jit.branch64(CCallHelpers::GreaterThanOrEqual, T11, TrustedImm32(0));
    jit.loadPtr(Address(R0, JSSymbolTableObject::offsetOfSymbolTable()), T13);
    miss.append(jit.branchPtr(CCallHelpers::NotEqual, T12, T13));
    jit.urshift64(T11, TrustedImm32(32), T11);
    jit.and32(TrustedImm32(Slot::offsetMask), T11);
    jit.load64(CCallHelpers::BaseIndex(R0, T11, CCallHelpers::TimesEight, JSLexicalEnvironment::offsetOfVariables()), T12);
    miss.append(jit.branchTest64(CCallHelpers::Zero, T12));
    jit.move(T12, R0);
    jit.ret();
    isAddress.link(&jit);
    jit.load64(Address(T12), T12);
    miss.append(jit.branchTest64(CCallHelpers::Zero, T12));
    jit.move(T12, R0);
    jit.ret();

    noAddress.link(&jit);
    jit.urshift64(T11, TrustedImm32(32), T11);
    Jump outOfLine = jit.branch32(CCallHelpers::GreaterThanOrEqual, T11, TrustedImm32(firstOutOfLineOffset));
    jit.load64(CCallHelpers::BaseIndex(R0, T11, CCallHelpers::TimesEight, JSObject::offsetOfInlineStorage()), R0);
    jit.ret();
    outOfLine.link(&jit);
    jit.loadPtr(Address(R0, JSObject::butterflyOffset()), T12);
    jit.neg64(T11);
    jit.load64(CCallHelpers::BaseIndex(T12, T11, CCallHelpers::TimesEight, (firstOutOfLineOffset - 2) * static_cast<int>(sizeof(EncodedJSValue))), R0);
    jit.ret();
}

static void generateGetFromScope(CCallHelpers& jit)
{
    CCallHelpers::JumpList miss;
    getFromScopeIfCached(jit, 0, miss);

    miss.link(&jit);
    missAtSite(jit, Entry::operationAOTGetFromScope, 1, Returns::Value);
}

static void generatePutToScope(CCallHelpers& jit)
{
    CCallHelpers::JumpList miss;
    jit.load64(slotWord(A2, 0), T11);
    miss.append(branchIfStructureDiffers(jit, R0, T11));
    jit.loadPtr(slotWord(A2, 1), T12);
    miss.append(jit.branchPtr(CCallHelpers::NotEqual, T12, Address(R0, JSSymbolTableObject::offsetOfSymbolTable())));
    jit.urshift64(T11, TrustedImm32(32), T11);
    jit.and32(TrustedImm32(Slot::offsetMask), T11);
    jit.store64(A1, CCallHelpers::BaseIndex(R0, T11, CCallHelpers::TimesEight, JSLexicalEnvironment::offsetOfVariables()));

    returnAfterStoring(jit, A1);

    miss.link(&jit);
    missAtSite(jit, Entry::operationAOTPutToScope, 2, Returns::Void);
}

static void generateGetGlobal(CCallHelpers& jit)
{
    static_assert(sizeof(Slot) == 2 * sizeof(void*));
    jit.load32(slotWord(A1, 0).withOffset(sizeof(uint32_t)), T11);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfGlobalObject()), T12);
    jit.load32(Address(T12, JSGlobalObject::offsetOfGlobalLexicalBindingEpoch()), T12);
    jit.add32(TrustedImm32(1), T12);
    Jump entryMismatch = jit.branch32(CCallHelpers::NotEqual, T11, T12);
    jit.loadPtr(slotWord(A1, 1), R0);

    auto resolved = jit.label();
    CCallHelpers::JumpList miss;
    getFromScopeIfCached(jit, 2, miss);

    miss.link(&jit);
    jit.addPtr(TrustedImm32(sizeof(Slot)), A1);
    missAtSite(jit, Entry::operationAOTGetFromScope, 1, Returns::Value);

    entryMismatch.link(&jit);
    Jump notResolved = walkScopeChainIfResolvedByDepth(jit);
    jit.jump().linkTo(resolved, &jit);

    notResolved.link(&jit);
    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(16), CCallHelpers::stackPointerRegister);
    jit.storePtr(A1, Address(CCallHelpers::stackPointerRegister));
    prepareMissAtSite(jit, Entry::operationAOTResolveScope, 1, 16 + sizeof(CPURegister));
    jit.call(T9, OperationPtrTag);
    jit.move(GPRInfo::returnValueGPR2, T11);
    jit.loadPtr(Address(CCallHelpers::stackPointerRegister), A1);
    jit.emitFunctionEpilogue();
    loadInstance(jit, T9);
    Jump exception = jit.branchTestPtr(CCallHelpers::NonZero, T11);
    jit.jump().linkTo(resolved, &jit);

    exception.link(&jit);
    jumpToEntry(jit, T9, Entry::HandleException);
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

static void generateThrowStackOverflowAtPrologue(CCallHelpers& jit)
{
    jit.emitFunctionEpilogue();
    throwStackOverflow(jit);
}

static void generateThrowStackOverflow(CCallHelpers& jit)
{
    throwStackOverflow(jit);
}

static void generateThrowCalledIndirectly(CCallHelpers& jit)
{
    jit.emitFunctionPrologue();
    jit.move(calleeGPR, A1);
    jit.move(instanceGPR, A0);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T9);
    jit.loadPtr(Address(T9, static_cast<unsigned>(Entry::ThrowCalledIndirectlyError) * sizeof(void*)), T9);
    jit.call(T9, OperationPtrTag);
    jit.emitFunctionEpilogue();
    generateHandleException(jit);
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
    static_assert(offsetOfInstanceRegisterInAdapter == offsetOfInstanceInAdapter + 8 && offsetOfNotCellMaskRegisterInAdapter == offsetOfNumberTagRegisterInAdapter + 8);
    jit.storePair64(instance, instanceGPR, GPRInfo::callFrameRegister, TrustedImm32(offsetOfInstanceInAdapter));
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
    jit.emitMaterializeTagCheckRegisters();
    jit.storePtr(CCallHelpers::TrustedImmPtr(nullptr), CCallHelpers::addressFor(CallFrameSlot::codeBlock));

    jit.load64(CCallHelpers::addressFor(CallFrameSlot::callee), calleeGPR);
#if CPU(ARM64)
    static_assert(static_cast<int>(CallFrameSlot::thisArgument) == static_cast<int>(CallFrameSlot::argumentCountIncludingThis) + 1);
    jit.loadPair64(GPRInfo::callFrameRegister, TrustedImm32(static_cast<int>(CallFrameSlot::argumentCountIncludingThis) * static_cast<int>(sizeof(Register))), countGPR, thisGPR);
#else
    jit.load64(CCallHelpers::addressFor(CallFrameSlot::thisArgument), thisGPR);
    jit.load32(CCallHelpers::lowWordFor(CallFrameSlot::argumentCountIncludingThis), countGPR);
#endif
    jit.sub32(TrustedImm32(1), countGPR);
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
    slow.append(jit.branchTestPtr(CCallHelpers::NonZero, T11, TrustedImm32(JSFunction::aotFunctionTag)));
    Jump hasExecutable = jit.branchTestPtr(CCallHelpers::Zero, T11, TrustedImm32(JSFunction::rareDataTag));
    jit.loadPtr(Address(T11, FunctionRareData::offsetOfExecutable() - JSFunction::rareDataTag), T11);
    hasExecutable.link(&jit);
    Jump isShortForm = jit.branchIfType(T11, ShortFunctionExecutableType);
    jit.loadPtr(Address(T11, ExecutableBase::offsetOfJITCodeWithArityCheckFor(kind)), T12);
    Jump hasEntrypoint = jit.branchTestPtr(CCallHelpers::NonZero, T12);
    isShortForm.link(&jit);
    jit.loadPtr(Address(T11, FunctionExecutable::offsetOfAOTEntryFor(kind)), T12);
    slow.append(jit.branchTestPtr(CCallHelpers::Zero, T12));
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T12);
    if (kind == CodeSpecializationKind::CodeForConstruct) {
        jit.load32(Address(T11, FunctionExecutable::offsetOfAOTIndexFor(kind)), T11);
        Jump doesNotConstructViaCall = jit.branch32(CCallHelpers::NotEqual, T11, TrustedImm32(static_cast<int32_t>(FunctionExecutable::aotConstructViaCallIndex)));
        jit.loadPtr(Address(T12, static_cast<unsigned>(Entry::ConstructViaCall) * sizeof(void*)), T12);
        jit.farJump(T12, JSEntryPtrTag);
        doesNotConstructViaCall.link(&jit);
    }
    jit.loadPtr(Address(T12, static_cast<unsigned>(isCall(kind) ? Entry::EnterStaticFunctionForCall : Entry::EnterStaticFunctionForConstruct) * sizeof(void*)), T12);
    jit.farJump(T12, JSEntryPtrTag);
    hasEntrypoint.link(&jit);
    Jump isNative = jit.branchIfNotType(T11, FunctionExecutableType);
    jit.loadPtr(Address(T11, FunctionExecutable::offsetOfCodeBlockFor(kind)), T11);
    jit.storePtr(T11, incomingFrameSlot(CallFrameSlot::codeBlock));
    isNative.link(&jit);
    jit.farJump(T12, JSEntryPtrTag);

    isNotFunction.link(&jit);
    slow.append(jit.branchIfNotType(BaselineJITRegisters::Call::calleeGPR, InternalFunctionType));
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T12);
    jit.loadPtr(Address(T12, static_cast<unsigned>(isCall(kind) ? Entry::InternalFunctionCallTrampoline : Entry::InternalFunctionConstructTrampoline) * sizeof(void*)), T12);
    jit.farJump(T12, JSEntryPtrTag);

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

static void generateEnterStaticFunction(CCallHelpers& jit, CodeSpecializationKind kind, Entry callLinkInfo)
{
    constexpr GPRReg T12 = entryT12;
    constexpr GPRReg T13 = entryT13;
    loadCalleeFrameCalleeAndVM(jit, T11, T12);
    jit.load32(Address(T11, JSCell::structureIDOffset()), T9);
    jit.add64(Address(T12, VM::offsetOfStructureIDBase()), T9);
    jit.loadPtr(Address(T9, Structure::offsetOfAOTInstance()), T12);
    Jump hasInstance = jit.branchTestPtr(CCallHelpers::NonZero, T12);
    jit.loadPtr(Address(T9, Structure::realmOffset()), T12);
    jit.loadPtr(Address(T12, JSGlobalObject::offsetOfAOTInstance()), T12);
    hasInstance.link(&jit);

    jit.loadPtr(Address(T11, JSFunction::offsetOfExecutableOrRareData()), T11);
    Jump hasFunctionWord = jit.branchTestPtr(CCallHelpers::NonZero, T11, TrustedImm32(JSFunction::aotFunctionTag));
    Jump hasExecutable = jit.branchTestPtr(CCallHelpers::Zero, T11, TrustedImm32(JSFunction::rareDataTag));
    jit.loadPtr(Address(T11, FunctionRareData::offsetOfExecutable() - JSFunction::rareDataTag), T11);
    hasExecutable.link(&jit);
    jit.load32(Address(T11, FunctionExecutable::offsetOfAOTIndexFor(kind)), T13);
    jit.loadPtr(Address(T11, FunctionExecutable::offsetOfAOTEntryFor(kind)), T11);
    Jump entryReady = jit.jump();
    hasFunctionWord.link(&jit);
    extractBits(jit, T11, JSFunction::aotFunctionIndexShift, JSFunction::aotFunctionIndexBits, T13);
    jit.and64(TrustedImm32(static_cast<int32_t>(JSFunction::aotCodeOffsetMask)), T11, T9);
    jit.and64(CCallHelpers::TrustedImm64(static_cast<int64_t>(JSFunction::aotEntryFlagsMask)), T11);
    jit.add64(T9, T11);
    entryReady.link(&jit);
    jit.addPtr(TrustedImm32(Instance::offsetOfStates()), T12, T9);
    jit.load16(CCallHelpers::BaseIndex(T9, T13, CCallHelpers::TimesTwo), T9);
    Jump isStillEmpty = jit.branchTest32(CCallHelpers::Zero, T9);
    jit.loadPtr(Address(T12, Instance::offsetOfCode()), T9);
    jit.add64(T9, T11);
    adapt(jit, T11, T12);

    isStillEmpty.link(&jit);
    jit.loadPtr(Address(T12, Instance::offsetOfRuntimeTable()), T11);
    jit.loadPtr(Address(T11, static_cast<unsigned>(callLinkInfo) * sizeof(void*)), BaselineJITRegisters::Call::callLinkInfoGPR);
    findTargetAndCall(jit);
}
static void generateEnterStaticFunctionForCall(CCallHelpers& jit) { generateEnterStaticFunction(jit, CodeSpecializationKind::CodeForCall, Entry::CallLinkInfoForCall); }
static void generateEnterStaticFunctionForConstruct(CCallHelpers& jit) { generateEnterStaticFunction(jit, CodeSpecializationKind::CodeForConstruct, Entry::CallLinkInfoForConstruct); }

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
    jit.loadPtr(Address(scratch, JSFunction::offsetOfExecutableOrRareData()), total);
    slowCase.append(jit.branchTestPtr(CCallHelpers::NonZero, total, TrustedImm32(JSFunction::aotFunctionTag)));
    Jump hasExecutable = jit.branchTestPtr(CCallHelpers::Zero, total, TrustedImm32(JSFunction::rareDataTag));
    jit.loadPtr(Address(total, FunctionRareData::offsetOfExecutable() - JSFunction::rareDataTag), total);
    hasExecutable.link(&jit);
    Jump isShortForm = jit.branchIfType(total, ShortFunctionExecutableType);
    jit.loadPtr(Address(total, ExecutableBase::offsetOfJITCodeWithArityCheckFor(CodeSpecializationKind::CodeForCall)), scratch);
    Jump hasEntrypoint = jit.branchTestPtr(CCallHelpers::NonZero, scratch);
    isShortForm.link(&jit);
    jit.loadPtr(Address(total, FunctionExecutable::offsetOfAOTEntryFor(CodeSpecializationKind::CodeForCall)), scratch);
    slowCase.append(jit.branchTestPtr(CCallHelpers::Zero, scratch));
    jit.loadPtr(Address(vm, VM::offsetOfAOTRuntimeTable()), scratch);
    jit.loadPtr(Address(scratch, static_cast<unsigned>(Entry::EnterStaticFunctionForCall) * sizeof(void*)), scratch);
    Jump hasKnownEntrypoint = jit.jump();
    hasEntrypoint.link(&jit);
    Jump isNative = jit.branchIfNotType(total, FunctionExecutableType);
    jit.loadPtr(Address(total, FunctionExecutable::offsetOfCodeBlockForCall()), passed);
    jit.storePtr(passed, CCallHelpers::calleeFrameCodeBlockBeforeCall());
    isNative.link(&jit);
    hasKnownEntrypoint.link(&jit);
    jit.call(scratch, JSEntryPtrTag);
    jit.emitFunctionEpilogue();
    jit.ret();

    slowCase.link(&jit);
    jit.emitFunctionEpilogue();
    jit.loadPtr(Address(vm, VM::offsetOfAOTRuntimeTable()), T11);
    jit.loadPtr(Address(T11, static_cast<unsigned>(Entry::NativeCallTrampoline) * sizeof(void*)), T11);
    jit.farJump(T11, JSEntryPtrTag);
}

static void generateConstructViaCall(CCallHelpers& jit)
{
    loadCalleeFrameCalleeAndVM(jit, T9, T10);
    jit.loadPtr(Address(T10, VM::offsetOfAOTRuntimeTable()), T11);
    jit.emitFunctionPrologue();
    jit.storePtr(CCallHelpers::TrustedImmPtr(nullptr), CCallHelpers::addressFor(CallFrameSlot::codeBlock));
    jit.subPtr(TrustedImm32(16), CCallHelpers::stackPointerRegister);
    jit.storePtr(T11, Address(CCallHelpers::stackPointerRegister));
    jit.move(GPRInfo::callFrameRegister, A0);
    jit.loadPtr(Address(T11, static_cast<unsigned>(Entry::operationAOTConstructViaCall) * sizeof(void*)), T9);
    jit.call(T9, OperationPtrTag);
    jit.loadPtr(Address(CCallHelpers::stackPointerRegister), T11);
    jit.addPtr(TrustedImm32(16), CCallHelpers::stackPointerRegister);
    Jump threw = jit.branchTestPtr(CCallHelpers::NonZero, GPRInfo::returnValueGPR2);
    jit.emitFunctionEpilogue();
    jit.ret();

    threw.link(&jit);
    jit.move(GPRInfo::returnValueGPR2, T10);
    jit.loadPtr(Address(T11, static_cast<unsigned>(Entry::LookupExceptionHandler) * sizeof(void*)), T11);
    unwind(jit, T10, T11);
}

static void generateLinkFunction(CCallHelpers& jit)
{
    callPreservingRegistersAndReturn(jit, Entry::operationAOTLinkFunction, false, [&] {
        jit.move(T10, A0);
        jit.move(T11, A1);
    });
}

#if CPU(X86_64)
static void generateConstant(CCallHelpers& jit)
{
    constexpr GPRReg place = GPRInfo::returnValueGPR;
    constexpr GPRReg keys = CCallHelpers::s_scratchRegister;
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfProgram()), T11);
    jit.loadPtr(Address(T11, VMProgram::offsetOfConstantKeys()), keys);
    jit.mul32(TrustedImm32(static_cast<int32_t>(VMProgram::constantHashMultiplier)), T9, place);
    jit.move(place, T10);
    jit.urshift32(TrustedImm32(VMProgram::constantHashShift), T10);
    jit.xor32(T10, place);
    jit.add32(TrustedImm32(1), T9, T10);
    CCallHelpers::Label next = jit.label();
    jit.and32(Address(T11, VMProgram::offsetOfConstantMask()), place);
    jit.load32(CCallHelpers::BaseIndex(keys, place, CCallHelpers::TimesFour), T9);
    Jump isThere = jit.branch32(CCallHelpers::Equal, T9, T10);
    Jump isNotMaterialized = jit.branchTest32(CCallHelpers::Zero, T9);
    jit.add32(TrustedImm32(1), place);
    jit.jump().linkTo(next, &jit);
    isThere.link(&jit);
    jit.loadPtr(Address(T11, VMProgram::offsetOfConstantValues()), T11);
    jit.load64(CCallHelpers::BaseIndex(T11, place, CCallHelpers::TimesEight), GPRInfo::returnValueGPR);
    jit.ret();
    isNotMaterialized.link(&jit);
    jit.move(T10, T9);
    jit.sub32(TrustedImm32(1), T9);
    callPreservingRegistersAndReturn(jit, Entry::operationAOTProgramConstant, true, [&] {
        jit.move(T9, A1);
        jit.move(T10, A0);
    });
}
#else
static void generateConstant(CCallHelpers& jit)
{
    constexpr GPRReg place = GPRInfo::returnValueGPR;
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfProgram()), T11);
    jit.loadPtr(Address(T11, VMProgram::offsetOfConstantValues()), T12);
    jit.loadPtr(Address(T11, VMProgram::offsetOfConstantKeys()), T13);
    jit.load32(Address(T11, VMProgram::offsetOfConstantMask()), T14);
    jit.move(TrustedImm32(static_cast<int32_t>(VMProgram::constantHashMultiplier)), T15);
    jit.mul32(T9, T15, place);
    jit.xorUnsignedRightShift32(place, place, TrustedImm32(VMProgram::constantHashShift), place);
    jit.add32(TrustedImm32(1), T9, T11);
    CCallHelpers::Label next = jit.label();
    jit.and32(T14, place);
    jit.load32(CCallHelpers::BaseIndex(T13, place, CCallHelpers::TimesFour), T15);
    Jump isThere = jit.branch32(CCallHelpers::Equal, T15, T11);
    Jump isNotMaterialized = jit.branchTest32(CCallHelpers::Zero, T15);
    jit.add32(TrustedImm32(1), place);
    jit.jump().linkTo(next, &jit);
    isThere.link(&jit);
    jit.load64(CCallHelpers::BaseIndex(T12, place, CCallHelpers::TimesEight), GPRInfo::returnValueGPR);
    jit.ret();
    isNotMaterialized.link(&jit);
    callPreservingRegistersAndReturn(jit, Entry::operationAOTProgramConstant, true, [&] {
        jit.move(T9, A1);
        jit.move(T10, A0);
    });
}
#endif

static void generateTemplateObject(CCallHelpers& jit)
{
    callPreservingRegistersAndReturn(jit, Entry::operationAOTTemplateObject, true, [&] {
        jit.move(T9, A1);
        jit.move(T10, A0);
    });
}

static void generateTransientConstant(CCallHelpers& jit)
{
    callPreservingRegistersAndReturn(jit, Entry::operationAOTCreateTransientConstant, true, [&] {
        jit.move(T9, A1);
        jit.move(T10, A0);
    });
}

static void generateConstantFromSlot(CCallHelpers& jit)
{
    constexpr GPRReg slot = T11;
    jit.load64(Address(slot, OBJECT_OFFSETOF(Slot, pointer)), GPRInfo::returnValueGPR);
    Jump isNotThere = jit.branchTest64(CCallHelpers::Zero, GPRInfo::returnValueGPR);
    jit.ret();

    isNotThere.link(&jit);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfSharedData()), GPRInfo::returnValueGPR);
#if CPU(X86_64)
    jit.move(slot, CCallHelpers::s_scratchRegister);
    jit.sub64(GPRInfo::returnValueGPR, CCallHelpers::s_scratchRegister);
    Jump hasOwnData = jit.branch64(CCallHelpers::AboveOrEqual, CCallHelpers::s_scratchRegister, TrustedImm32(SharedData::size));
#else
    jit.subPtr(slot, GPRInfo::returnValueGPR, CCallHelpers::memoryTempRegister);
    Jump hasOwnData = jit.branchPtr(CCallHelpers::AboveOrEqual, CCallHelpers::memoryTempRegister, CCallHelpers::TrustedImmPtr(SharedData::size));
#endif
    s_callsBetweenStubs->append({ jit.nearTailCall(), Stub::Constant });

    hasOwnData.link(&jit);
#if CPU(ARM64)
    jit.pushPair(slot, CCallHelpers::linkRegister);
#else
    jit.push(slot);
#endif
    callStubFromStub(jit, Stub::Constant);
#if CPU(ARM64)
    jit.popPair(slot, CCallHelpers::linkRegister);
#else
    jit.pop(slot);
#endif
    jit.store64(GPRInfo::returnValueGPR, Address(slot, OBJECT_OFFSETOF(Slot, pointer)));
    jit.ret();
}

static void generateVirtualCall(CCallHelpers& jit) { dispatchCall(jit, CodeSpecializationKind::CodeForCall, [] { }); }
static void generateVirtualConstruct(CCallHelpers& jit) { dispatchCall(jit, CodeSpecializationKind::CodeForConstruct, [] { }); }
static void generateVirtualTailCall(CCallHelpers& jit) { dispatchCall(jit, CodeSpecializationKind::CodeForCall, [] { }); }

static_assert(JSFunction::aotEntryFlagsShift == EntryWord::numberOfAddressBits);

#if CPU(X86_64)
constexpr GPRReg callTargetGPR = T11;
constexpr GPRReg callTemporaryGPR = T13;
constexpr GPRReg listCallTemporaryGPR = A2;
constexpr GPRReg listTailCallTemporaryGPR = T9;

static void findCalleeCode(CCallHelpers& jit, CodeSpecializationKind kind, CCallHelpers::JumpList& otherwise)
{
    auto checkInstance = [&] {
        jit.load32(Address(calleeGPR, JSCell::structureIDOffset()), T13);
        structureWithID(jit, T13);
        Jump belongsToThisInstance = jit.branchPtr(CCallHelpers::Equal, Address(T13, Structure::offsetOfAOTInstance()), instanceGPR);
        otherwise.append(jit.branchTestPtr(CCallHelpers::NonZero, Address(T13, Structure::offsetOfAOTInstance())));
        jit.loadPtr(Address(T13, Structure::realmOffset()), T13);
        otherwise.append(jit.branchPtr(CCallHelpers::NotEqual, Address(instanceGPR, Instance::offsetOfGlobalObject()), T13));
        belongsToThisInstance.link(&jit);
    };
    auto checkIsLinked = [&](GPRReg index) {
        jit.load16(CCallHelpers::BaseIndex(instanceGPR, index, CCallHelpers::TimesTwo, Instance::offsetOfStates()), index);
        otherwise.append(jit.branchTest32(CCallHelpers::Zero, index));
    };
    otherwise.append(jit.branchIfNotCell(calleeGPR));
    otherwise.append(jit.branchIfNotType(calleeGPR, JSFunctionType));
    jit.loadPtr(Address(calleeGPR, JSFunction::offsetOfExecutableOrRareData()), T11);
    Jump hasFunctionWord = jit.branchTestPtr(CCallHelpers::NonZero, T11, TrustedImm32(JSFunction::aotFunctionTag));
    Jump hasExecutable = jit.branchTestPtr(CCallHelpers::Zero, T11, TrustedImm32(JSFunction::rareDataTag));
    jit.loadPtr(Address(T11, FunctionRareData::offsetOfExecutable() - JSFunction::rareDataTag), T11);
    hasExecutable.link(&jit);
    otherwise.append(jit.branchIfNotType(T11, JSTypeRange { FunctionExecutableType, ShortFunctionExecutableType }));
    Jump isNotStatic = jit.branchTest64(CCallHelpers::Zero, Address(T11, FunctionExecutable::offsetOfAOTEntryFor(kind)));
    checkInstance();
    jit.load32(Address(T11, FunctionExecutable::offsetOfAOTIndexFor(kind)), T13);
    if (kind == CodeSpecializationKind::CodeForConstruct)
        otherwise.append(jit.branch32(CCallHelpers::Equal, T13, TrustedImm32(static_cast<int32_t>(FunctionExecutable::aotConstructViaCallIndex))));
    checkIsLinked(T13);
    jit.load64(Address(T11, FunctionExecutable::offsetOfAOTEntryFor(kind)), callTargetGPR);
    jit.add64(Address(instanceGPR, Instance::offsetOfCode()), callTargetGPR);
    Jump isLinked = jit.jump();

    isNotStatic.link(&jit);
    otherwise.append(jit.branchIfNotType(T11, FunctionExecutableType));
    jit.loadPtr(Address(T11, ExecutableBase::offsetOfJITCodeFor(kind)), T11);
    otherwise.append(jit.branchTestPtr(CCallHelpers::Zero, T11));
    otherwise.append(jit.branch8(CCallHelpers::NotEqual, Address(T11, JSC::JITCode::offsetOfJITType()), TrustedImm32(static_cast<int32_t>(JITType::AOTJIT))));
    otherwise.append(jit.branchPtr(CCallHelpers::NotEqual, Address(T11, JITCode::offsetOfInstance()), instanceGPR));
    jit.load64(Address(T11, JITCode::offsetOfEntry()), callTargetGPR);
    Jump isInstalled = jit.jump();

    hasFunctionWord.link(&jit);
    if (kind == CodeSpecializationKind::CodeForConstruct)
        otherwise.append(jit.jump());
    else {
        checkInstance();
        extractBits(jit, T11, JSFunction::aotFunctionIndexShift, JSFunction::aotFunctionIndexBits, T13);
        checkIsLinked(T13);
        jit.and64(TrustedImm32(static_cast<int32_t>(JSFunction::aotCodeOffsetMask)), T11, T13);
        jit.and64(CCallHelpers::TrustedImm64(static_cast<int64_t>(JSFunction::aotEntryFlagsMask)), T11);
        jit.add64(T13, callTargetGPR);
        jit.add64(Address(instanceGPR, Instance::offsetOfCode()), callTargetGPR);
    }
    isInstalled.link(&jit);
    isLinked.link(&jit);
}
#else
constexpr GPRReg callTargetGPR = T12;
constexpr GPRReg callTemporaryGPR = T11;
constexpr GPRReg listCallTemporaryGPR = T12;
constexpr GPRReg listTailCallTemporaryGPR = T12;

static void findCalleeCode(CCallHelpers& jit, CodeSpecializationKind kind, CCallHelpers::JumpList& otherwise)
{
    otherwise.append(jit.branchIfNotCell(calleeGPR));
    otherwise.append(jit.branchIfNotType(calleeGPR, JSFunctionType));
    jit.loadPtr(Address(calleeGPR, JSFunction::offsetOfExecutableOrRareData()), T11);
    Jump hasFunctionWord = jit.branchTestPtr(CCallHelpers::NonZero, T11, TrustedImm32(JSFunction::aotFunctionTag));
    Jump hasExecutable = jit.branchTestPtr(CCallHelpers::Zero, T11, TrustedImm32(JSFunction::rareDataTag));
    jit.loadPtr(Address(T11, FunctionRareData::offsetOfExecutable() - JSFunction::rareDataTag), T11);
    hasExecutable.link(&jit);
    otherwise.append(jit.branchIfNotType(T11, JSTypeRange { FunctionExecutableType, ShortFunctionExecutableType }));
    jit.load64(Address(T11, FunctionExecutable::offsetOfAOTEntryFor(kind)), T12);
    Jump isNotStatic = jit.branchTest64(CCallHelpers::Zero, T12);
    jit.load32(Address(calleeGPR, JSCell::structureIDOffset()), T13);
    structureWithID(jit, T13);
    Jump belongsToThisInstance = jit.branchPtr(CCallHelpers::Equal, Address(T13, Structure::offsetOfAOTInstance()), instanceGPR);
    otherwise.append(jit.branchTestPtr(CCallHelpers::NonZero, Address(T13, Structure::offsetOfAOTInstance())));
    jit.loadPtr(Address(T13, Structure::realmOffset()), T13);
    otherwise.append(jit.branchPtr(CCallHelpers::NotEqual, Address(instanceGPR, Instance::offsetOfGlobalObject()), T13));
    belongsToThisInstance.link(&jit);
    jit.load32(Address(T11, FunctionExecutable::offsetOfAOTIndexFor(kind)), T13);
    if (kind == CodeSpecializationKind::CodeForConstruct)
        otherwise.append(jit.branch32(CCallHelpers::Equal, T13, TrustedImm32(static_cast<int32_t>(FunctionExecutable::aotConstructViaCallIndex))));
    jit.addPtr(TrustedImm32(Instance::offsetOfStates()), instanceGPR, T11);
    jit.load16(CCallHelpers::BaseIndex(T11, T13, CCallHelpers::TimesTwo), T11);
    otherwise.append(jit.branchTest32(CCallHelpers::Zero, T11));
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfCode()), T11);
    jit.add64(T11, T12);
    Jump isLinked = jit.jump();

    isNotStatic.link(&jit);
    otherwise.append(jit.branchIfNotType(T11, FunctionExecutableType));
    jit.loadPtr(Address(T11, ExecutableBase::offsetOfJITCodeFor(kind)), T11);
    otherwise.append(jit.branchTestPtr(CCallHelpers::Zero, T11));
    otherwise.append(jit.branch8(CCallHelpers::NotEqual, Address(T11, JSC::JITCode::offsetOfJITType()), TrustedImm32(static_cast<int32_t>(JITType::AOTJIT))));
    otherwise.append(jit.branchPtr(CCallHelpers::NotEqual, Address(T11, JITCode::offsetOfInstance()), instanceGPR));
    jit.load64(Address(T11, JITCode::offsetOfEntry()), T12);
    Jump isInstalled = jit.jump();

    hasFunctionWord.link(&jit);
    if (kind == CodeSpecializationKind::CodeForConstruct)
        otherwise.append(jit.jump());
    else {
        jit.load32(Address(calleeGPR, JSCell::structureIDOffset()), T13);
        structureWithID(jit, T13);
        Jump belongsToThisInstance = jit.branchPtr(CCallHelpers::Equal, Address(T13, Structure::offsetOfAOTInstance()), instanceGPR);
        otherwise.append(jit.branchTestPtr(CCallHelpers::NonZero, Address(T13, Structure::offsetOfAOTInstance())));
        jit.loadPtr(Address(T13, Structure::realmOffset()), T13);
        otherwise.append(jit.branchPtr(CCallHelpers::NotEqual, Address(instanceGPR, Instance::offsetOfGlobalObject()), T13));
        belongsToThisInstance.link(&jit);
        extractBits(jit, T11, JSFunction::aotFunctionIndexShift, JSFunction::aotFunctionIndexBits, T13);
        jit.addPtr(TrustedImm32(Instance::offsetOfStates()), instanceGPR, T12);
        jit.load16(CCallHelpers::BaseIndex(T12, T13, CCallHelpers::TimesTwo), T12);
        otherwise.append(jit.branchTest32(CCallHelpers::Zero, T12));
        jit.and64(TrustedImm32(static_cast<int32_t>(JSFunction::aotCodeOffsetMask)), T11, T12);
        jit.and64(CCallHelpers::TrustedImm64(static_cast<int64_t>(JSFunction::aotEntryFlagsMask)), T11);
        jit.add64(T11, T12);
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfCode()), T11);
        jit.add64(T11, T12);
    }
    isInstalled.link(&jit);
    isLinked.link(&jit);
}
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
    jit.jump().linkTo(variadicCallStart(), &jit);
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
    jit.jump().linkTo(variadicCallStart(), &jit);
    passesNothing.link(&jit);
    jit.move(CCallHelpers::TrustedImm64(JSValue::ValueUndefined), thisGPR);
    jit.jump().linkTo(variadicCallStart(), &jit);
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
    jit.jump().linkTo(variadicCallStart(), &jit);
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

struct HostCallThunks {
    CCallHelpers::Label callHostFunction;
    CCallHelpers::Label callInternalFunction;
    CCallHelpers::Label callCached;
};

// Indirect call of a closure of the typed compiler: the entry point is in the cell. Falls through if the callee is not such a closure of
// this instance, or is a class. With a static count, countGPR is not read. Jumps to `takesList` with the entry word in callTargetGPR if the
// entry point takes its arguments in memory.
static void callTypedClosure(CCallHelpers& jit, std::optional<unsigned> count, CCallHelpers::JumpList& takesList)
{
    CCallHelpers::JumpList isNotClosure;
    isNotClosure.append(jit.branchIfNotCell(calleeGPR));
    jit.load32(Address(calleeGPR, JSCell::structureIDOffset()), T11);
    Jump isClosure = jit.branch32(CCallHelpers::Equal, T11, Address(instanceGPR, Instance::offsetOfTypedClosureStructureID(0)));
    // An async function or a generator function has another structure, and so has a closure that was given a property (`f.prototype`,
    // `f.displayName = ..`).
    isNotClosure.append(jit.branchIfNotType(calleeGPR, JSFunctionType));
    structureWithID(jit, T11);
    isNotClosure.append(jit.branchPtr(CCallHelpers::NotEqual, Address(T11, Structure::offsetOfAOTInstance()), instanceGPR));
    jit.load32(Address(instanceGPR, Instance::offsetOfTypedClosureStructureID(0)), T13);
    structureWithID(jit, T13);
    jit.loadPtr(Address(T13, Structure::classInfoOffset()), T13);
    isNotClosure.append(jit.branchPtr(CCallHelpers::NotEqual, Address(T11, Structure::classInfoOffset()), T13));
    isClosure.link(&jit);
    jit.load64(Address(calleeGPR, Typed::Closure::offsetOfEntry()), callTargetGPR);
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
    // Where it goes on JITCode::m_entry goes through as well: of the word only the address is looked at there.
    toAddressOfCode(jit, callTargetGPR, T13);
    takesList.append(jit.jump());
    isNotClosure.link(&jit);
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

static void generateCallTo(CCallHelpers& jit, CodeSpecializationKind kind, std::optional<unsigned> count, const HostCallThunks* thunksToCache = nullptr)
{
    bool isCached = thunksToCache;
    if (kind == CodeSpecializationKind::CodeForCall && !count)
        variadicCallStart() = jit.label();
    CCallHelpers::JumpList closureTakesList;
    if (kind == CodeSpecializationKind::CodeForCall && !isCached)
        callTypedClosure(jit, count, closureTakesList);
    if (count && !isCached)
        jit.move(TrustedImm32(*count), countGPR);
    CCallHelpers::JumpList slowCase;
    if (isCached && (Options::useAOTOperationCounters() || Options::useAOTTypeCoverageCounters())) {
        preservingRegistersOfCaller(jit, false, [&](unsigned) {
            jit.move(calleeGPR, A2);
            jit.move(countGPR, A1);
            jit.move(instanceGPR, A0);
            jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T9);
            jit.loadPtr(Address(T9, static_cast<unsigned>(Entry::operationAOTNoteCallee) * sizeof(void*)), T9);
            jit.call(T9, OperationPtrTag);
        });
    }
    findCalleeCode(jit, kind, slowCase);
    bool countsMisses = isCached && (Options::useAOTOperationCounters() || Options::useAOTTypeCoverageCounters());
    auto countMiss = [&](Instance::CalleeCacheMiss which) {
        jit.add64(TrustedImm32(1), Address(instanceGPR, Instance::offsetOfCalleeCacheMisses(which)));
    };
    if (countsMisses) {
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfSharedData()), T13);
#if CPU(X86_64)
        jit.move(countGPR, CCallHelpers::s_scratchRegister);
        jit.sub64(T13, CCallHelpers::s_scratchRegister);
        Jump isShared = jit.branch64(CCallHelpers::Below, CCallHelpers::s_scratchRegister, TrustedImm32(SharedData::size));
#else
        jit.subPtr(countGPR, T13, CCallHelpers::memoryTempRegister);
        Jump isShared = jit.branchPtr(CCallHelpers::Below, CCallHelpers::memoryTempRegister, CCallHelpers::TrustedImmPtr(SharedData::size));
#endif
        jit.move(callTargetGPR, T13);
        jit.and64(CCallHelpers::TrustedImm64(static_cast<int64_t>(EntryWord::addressMask)), T13);
        Jump isOtherCode = jit.branchPtr(CCallHelpers::NotEqual, Address(countGPR, sizeof(Slot) + OBJECT_OFFSETOF(Slot, pointer)), T13);
        countMiss(Instance::CalleeCacheMiss::SameCode);
        Jump isCountedAsSameCode = jit.jump();
        isOtherCode.link(&jit);
        countMiss(Instance::CalleeCacheMiss::OtherCode);
        Jump isCountedAsOtherCode = jit.jump();
        isShared.link(&jit);
        countMiss(Instance::CalleeCacheMiss::CacheInSharedData);
        isCountedAsSameCode.link(&jit);
        isCountedAsOtherCode.link(&jit);
    }
    auto fillCacheUnlessFirstMiss = [&](Entry operation, const auto& passThirdAndFourthOperands) {
        Address state(countGPR, sizeof(Slot) + OBJECT_OFFSETOF(Slot, offset));
        Jump hasGivenUp = jit.branchTest32(CCallHelpers::NonZero, state, TrustedImm32(CalleeCache::hasGivenUp));
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfSharedData()), T13);
#if CPU(X86_64)
        jit.move(countGPR, CCallHelpers::s_scratchRegister);
        jit.sub64(T13, CCallHelpers::s_scratchRegister);
        Jump isShared = jit.branch64(CCallHelpers::Below, CCallHelpers::s_scratchRegister, TrustedImm32(SharedData::size));
#else
        jit.subPtr(countGPR, T13, CCallHelpers::memoryTempRegister);
        Jump isShared = jit.branchPtr(CCallHelpers::Below, CCallHelpers::memoryTempRegister, CCallHelpers::TrustedImmPtr(SharedData::size));
#endif
        Jump hasMissedBefore = jit.branchTest32(CCallHelpers::NonZero, state);
        jit.store32(TrustedImm32(CalleeCache::attempt), state);
        Jump isFirstMiss = jit.jump();
        hasMissedBefore.link(&jit);
        preservingRegistersOfCaller(jit, false, [&](unsigned) {
            jit.move(calleeGPR, A2);
            passThirdAndFourthOperands();
            jit.move(countGPR, A1);
            jit.move(instanceGPR, A0);
            jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T9);
            jit.loadPtr(Address(T9, static_cast<unsigned>(operation) * sizeof(void*)), T9);
            jit.call(T9, OperationPtrTag);
        });
        Jump isFilled = jit.jump();
        isShared.link(&jit);
        CCallHelpers::JumpList isNotCalledByOwnerOfCache;
#if CPU(ARM64)
        loadLabelAddress(jit, thunksToCache->callCached, T13);
        jit.load32(Address(CCallHelpers::linkRegister, -static_cast<int>(sizeof(uint32_t))), T14);
        jit.subPtr(T13, CCallHelpers::linkRegister, T13);
        jit.addPtr(TrustedImm32(sizeof(uint32_t)), T13);
        jit.extractUnsignedBitfield64(T13, TrustedImm32(2), TrustedImm32(26), T13);
        jit.or32(TrustedImm32(static_cast<int32_t>(0x94000000)), T13);
        isNotCalledByOwnerOfCache.append(jit.branch32(CCallHelpers::NotEqual, T13, T14));
#else
        jit.loadPtr(Address(CCallHelpers::stackPointerRegister), T13);
        isNotCalledByOwnerOfCache.append(jit.branch8(CCallHelpers::NotEqual, Address(T13, -static_cast<int>(sizeOfNearCall)), TrustedImm32(0xe8)));
        jit.load32(Address(T13, -static_cast<int>(sizeof(int32_t))), CCallHelpers::s_scratchRegister);
        jit.signExtend32ToPtr(CCallHelpers::s_scratchRegister, CCallHelpers::s_scratchRegister);
        jit.addPtr(T13, CCallHelpers::s_scratchRegister);
        loadLabelAddress(jit, thunksToCache->callCached, T13);
        isNotCalledByOwnerOfCache.append(jit.branchPtr(CCallHelpers::NotEqual, CCallHelpers::s_scratchRegister, T13));
#endif
        preservingRegistersOfCaller(jit, false, [&](unsigned bytes) {
            loadReturnAddressOfCaller(jit, bytes, A1);
            jit.move(instanceGPR, A0);
            jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T9);
            jit.loadPtr(Address(T9, static_cast<unsigned>(Entry::operationAOTCountMissOfCalleeCache) * sizeof(void*)), T9);
            jit.call(T9, OperationPtrTag);
        });
        isNotCalledByOwnerOfCache.link(&jit);
        isFilled.link(&jit);
        hasGivenUp.link(&jit);
        isFirstMiss.link(&jit);
        jit.move(TrustedImm32(*count), countGPR);
    };
    if (isCached) {
        fillCacheUnlessFirstMiss(Entry::operationAOTCacheCallee, [&] {
            jit.move(callTargetGPR, A3);
            jit.move(TrustedImm32(*count), A4);
        });
    }
    Jump takesList = jit.branchTest64(CCallHelpers::NonZero, callTargetGPR, CCallHelpers::TrustedImm64(1LL << EntryWord::isListBit));
    jit.urshift64(callTargetGPR, TrustedImm32(EntryWord::numberOfParametersShift), T13);
    jit.and64(CCallHelpers::TrustedImm64(static_cast<int64_t>(EntryWord::addressMask)), callTargetGPR);
    Jump enough = count ? jit.branch32(CCallHelpers::BelowOrEqual, T13, TrustedImm32(*count)) : jit.branch32(CCallHelpers::BelowOrEqual, T13, countGPR);
    jit.move(CCallHelpers::TrustedImm64(JSValue::ValueUndefined), T13);
    for (unsigned i = count.value_or(0); i < numberOfArgumentGPRs; ++i) {
        if (count)
            jit.move(T13, argumentGPR(i));
        else
            jit.moveConditionally32(CCallHelpers::Above, countGPR, TrustedImm32(i), argumentGPR(i), T13, argumentGPR(i));
    }
    enough.link(&jit);
    jit.farJump(callTargetGPR, JSEntryPtrTag);

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
        closureTakesList.link(&jit);
        if (count)
            jit.move(TrustedImm32(*count), countGPR);
    }
    takesList.link(&jit);
    CCallHelpers::Label callWithList = jit.label();
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

    slowCase.link(&jit);
    if (countsMisses)
        countMiss(Instance::CalleeCacheMiss::OtherKindOfCallee);
    if (isCached) {
        // The cache goes by the cell, and a callback is a new closure every time. Without this the call takes the engine's slow path.
        CCallHelpers::JumpList typedClosureTakesList;
        callTypedClosure(jit, count, typedClosureTakesList);
        Jump isNotTypedClosure = jit.jump();
        typedClosureTakesList.link(&jit);
        jit.move(TrustedImm32(*count), countGPR);
        jit.jump().linkTo(callWithList, &jit);
        isNotTypedClosure.link(&jit);
        fillCacheUnlessFirstMiss(Entry::operationAOTCacheHostCallee, [&] {
            loadLabelAddress(jit, thunksToCache->callHostFunction, A3);
            loadLabelAddress(jit, thunksToCache->callInternalFunction, A4);
        });
    }
    if (kind == CodeSpecializationKind::CodeForCall)
        callBoundTarget(jit);
    spill();
    callWithEngineConvention(jit, kind);
}

static void generateCallListTo(CCallHelpers& jit, CodeSpecializationKind kind)
{
    CCallHelpers::JumpList slowCase;
    Jump isTypedClosure;
    if (kind == CodeSpecializationKind::CodeForCall) {
        // A closure of the typed compiler with the structure it was allocated with, and no class.
        static_assert(noOverlap(T13, callTargetGPR, calleeGPR, thisGPR, argumentGPR(0), argumentGPR(1)));
        CCallHelpers::JumpList isNot;
        isNot.append(jit.branchIfNotCell(calleeGPR));
        jit.load32(Address(calleeGPR, JSCell::structureIDOffset()), T13);
        isNot.append(jit.branch32(CCallHelpers::NotEqual, T13, Address(instanceGPR, Instance::offsetOfTypedClosureStructureID(0))));
        jit.load64(Address(calleeGPR, Typed::Closure::offsetOfEntry()), callTargetGPR);
        isNot.append(jit.branchTest64(CCallHelpers::NonZero, callTargetGPR, CCallHelpers::TrustedImm64(static_cast<int64_t>(1ULL << ClosureWord::isNotCallableBit))));
        Jump closureTakesRegisters = jit.branchTest64(CCallHelpers::Zero, callTargetGPR, CCallHelpers::TrustedImm64(1LL << ClosureWord::isListBit));
        toAddressOfCode(jit, callTargetGPR, T13);
        jit.farJump(callTargetGPR, JSEntryPtrTag);
        closureTakesRegisters.link(&jit);
        toAddressOfCode(jit, callTargetGPR, T13);
        isTypedClosure = jit.jump();
        isNot.link(&jit);
    }
    findCalleeCode(jit, kind, slowCase);
    Jump takesRegisters = jit.branchTest64(CCallHelpers::Zero, callTargetGPR, CCallHelpers::TrustedImm64(1LL << EntryWord::isListBit));
    jit.and64(CCallHelpers::TrustedImm64(static_cast<int64_t>(EntryWord::addressMask)), callTargetGPR);
    jit.farJump(callTargetGPR, JSEntryPtrTag);

    takesRegisters.link(&jit);
    jit.and64(CCallHelpers::TrustedImm64(static_cast<int64_t>(EntryWord::addressMask)), callTargetGPR);
    // With the address of its code.
    if (isTypedClosure.isSet())
        isTypedClosure.link(&jit);
    jit.move(argumentGPR(0), countGPR);
    jit.move(argumentGPR(1), T13);
    Jump noMoreArguments[numberOfArgumentGPRs];
    for (unsigned i = 0; i < numberOfArgumentGPRs; ++i) {
        noMoreArguments[i] = jit.branch32(CCallHelpers::BelowOrEqual, countGPR, TrustedImm32(i));
        jit.load64(Address(T13, i * sizeof(Register)), argumentGPR(i));
    }
    jit.farJump(callTargetGPR, JSEntryPtrTag);
    for (unsigned i = 0; i < numberOfArgumentGPRs; ++i) {
        noMoreArguments[i].link(&jit);
        jit.move(CCallHelpers::TrustedImm64(JSValue::ValueUndefined), argumentGPR(i));
    }
    jit.farJump(callTargetGPR, JSEntryPtrTag);

    slowCase.link(&jit);
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

static void callListInTailPosition(CCallHelpers& jit)
{
    constexpr GPRReg length = A0;
    constexpr GPRReg to = A1;
    constexpr GPRReg from = A2;
    constexpr GPRReg frame = A3;
    static_assert(length == argumentGPR(0) && to == argumentGPR(1));
    jit.move(argumentGPR(1), from);
    jit.loadPtr(Address(GPRInfo::callFrameRegister), frame);
    loadLabelAddress(jit, returnFromCallWithList(), T11);
    CCallHelpers::Label again = jit.label();
    jit.loadPtr(Address(frame, sizeof(void*)), listTailCallTemporaryGPR);
    Jump isOutsideThisStub = jit.branchPtr(CCallHelpers::NotEqual, listTailCallTemporaryGPR, T11);
    jit.loadPtr(Address(frame), frame);
    jit.jump().linkTo(again, &jit);
    isOutsideThisStub.link(&jit);
    jit.add64(TrustedImm32(1), length, listTailCallTemporaryGPR);
    jit.and64(TrustedImm32(~1), listTailCallTemporaryGPR);
    jit.lshift64(TrustedImm32(3), listTailCallTemporaryGPR);
    jit.subPtr(frame, listTailCallTemporaryGPR, to);
    jit.move(length, listTailCallTemporaryGPR);
    Jump none = jit.branchTest64(CCallHelpers::Zero, listTailCallTemporaryGPR);
    CCallHelpers::Label next = jit.label();
    jit.sub64(TrustedImm32(1), listTailCallTemporaryGPR);
    jit.load64(CCallHelpers::BaseIndex(from, listTailCallTemporaryGPR, CCallHelpers::TimesEight), T13);
    jit.store64(T13, CCallHelpers::BaseIndex(to, listTailCallTemporaryGPR, CCallHelpers::TimesEight));
    jit.branchTest64(CCallHelpers::NonZero, listTailCallTemporaryGPR).linkTo(next, &jit);
    none.link(&jit);
    jit.move(frame, GPRInfo::callFrameRegister);
    jit.move(to, CCallHelpers::stackPointerRegister);
    jit.jump().linkTo(callVarargsReturnAddress(), &jit);
}

static void generateCallVarargsTo(CCallHelpers& jit, CodeSpecializationKind kind, bool inTailPosition = false)
{
    constexpr ptrdiff_t offsetOfCallee = -8;
    constexpr ptrdiff_t offsetOfThis = -16;
    constexpr ptrdiff_t offsetOfList = -24;
    constexpr ptrdiff_t offsetOfFirst = -32;
    constexpr ptrdiff_t offsetOfLength = -40;
    auto local = [](ptrdiff_t offset) { return Address(GPRInfo::callFrameRegister, offset); };
    auto callOperation = [&](Entry operation) {
        jit.move(instanceGPR, A0);
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T11);
        jit.loadPtr(Address(T11, static_cast<unsigned>(operation) * sizeof(void*)), T11);
        jit.call(T11, OperationPtrTag);
    };
    CCallHelpers::JumpList exception;
    CCallHelpers::JumpList overflow;

    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(48), CCallHelpers::stackPointerRegister);
    jit.store64(calleeGPR, local(offsetOfCallee));
    jit.store64(thisGPR, local(offsetOfThis));
    jit.store64(argumentGPR(0), local(offsetOfList));
    jit.store64(argumentGPR(1), local(offsetOfFirst));

    constexpr GPRReg lengthOrDestination = A2;
    constexpr GPRReg remaining = A3;
    constexpr GPRReg cursor = T10;
    constexpr GPRReg kinds = T9;
    constexpr GPRReg scratch = T11;
    constexpr GPRReg scratch2 = T13;
#if CPU(X86_64)
    constexpr GPRReg scratch3 = R0;
#else
    constexpr GPRReg scratch3 = T12;
#endif
    static_assert(noOverlap(argumentGPR(0), argumentGPR(1), lengthOrDestination, remaining, cursor, kinds, scratch, scratch2, scratch3));
    CCallHelpers::JumpList needsOperations;
    auto forEachItem = [&](const auto& onValue, const auto& onSpread, const auto& onPassed) {
        jit.load32(local(offsetOfFirst), kinds);
        Jump describesItems = jit.branchTest32(CCallHelpers::NonZero, kinds, TrustedImm32(1));
        needsOperations.append(jit.branchTest32(CCallHelpers::NonZero, kinds));
        jit.addPtr(TrustedImm32(offsetOfList), GPRInfo::callFrameRegister, cursor);
        jit.move(TrustedImm32(1), remaining);
        jit.move(TrustedImm32(ListDescriptor::Spread), kinds);
        Jump isPrepared = jit.jump();
        describesItems.link(&jit);
        jit.loadPtr(local(offsetOfList), cursor);
        jit.urshift32(kinds, TrustedImm32(1), remaining);
        jit.and32(TrustedImm32(15), remaining);
        jit.urshift32(TrustedImm32(5), kinds);
        isPrepared.link(&jit);

        CCallHelpers::Label next = jit.label();
        Jump done = jit.branchTest32(CCallHelpers::Zero, remaining);
        jit.sub32(TrustedImm32(1), remaining);
        jit.and32(TrustedImm32(3), kinds, scratch);
        jit.urshift32(TrustedImm32(2), kinds);
        Jump isSpread = jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(ListDescriptor::Spread));
        Jump isPassed = jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(ListDescriptor::Passed));
        onValue();
        jit.addPtr(TrustedImm32(sizeof(EncodedJSValue)), cursor);
        jit.jump().linkTo(next, &jit);
        isSpread.link(&jit);
        onSpread();
        jit.addPtr(TrustedImm32(sizeof(EncodedJSValue)), cursor);
        jit.jump().linkTo(next, &jit);
        isPassed.link(&jit);
        onPassed();
        jit.addPtr(TrustedImm32(2 * sizeof(EncodedJSValue)), cursor);
        jit.jump().linkTo(next, &jit);
        done.link(&jit);
    };

    needsOperations.append(jit.branchTest32(CCallHelpers::Zero, Address(instanceGPR, Instance::offsetOfArraysLackInheritedElements())));
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfGlobalObject()), scratch);
    jit.loadPtr(Address(scratch, JSGlobalObject::offsetOfArrayIteratorProtocolWatchpointSet() + InlineWatchpointSet::offsetOfData()), scratch);
    needsOperations.append(jit.branchPtr(CCallHelpers::Equal, scratch, CCallHelpers::TrustedImmPtr(InlineWatchpointSet::encodeState(IsInvalidated))));
    Jump isThin = jit.branchTestPtr(CCallHelpers::NonZero, scratch, TrustedImm32(InlineWatchpointSet::IsThinFlag));
    jit.load8(Address(scratch, WatchpointSet::offsetOfState()), scratch);
    needsOperations.append(jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(IsInvalidated)));
    isThin.link(&jit);

    jit.move(TrustedImm32(0), lengthOrDestination);
    forEachItem([&] {
        jit.add64(TrustedImm32(1), lengthOrDestination);
    }, [&] {
        jit.load64(Address(cursor), scratch2);
        needsOperations.append(jit.branchIfNotCell(scratch2));
        jit.load8(Address(scratch2, JSCell::indexingTypeAndMiscOffset()), scratch);
        jit.and32(TrustedImm32(IndexingShapeMask), scratch, scratch3);
        Jump isContiguous = jit.branch32(CCallHelpers::Equal, scratch3, TrustedImm32(ContiguousShape));
        needsOperations.append(jit.branch32(CCallHelpers::NotEqual, scratch3, TrustedImm32(Int32Shape)));
        isContiguous.link(&jit);
        jit.urshift32(TrustedImm32(Instance::arrayKindShift), scratch);
        jit.and32(TrustedImm32(Instance::numberOfArrayKinds - 1), scratch);
        jit.load32(CCallHelpers::BaseIndex(instanceGPR, scratch, CCallHelpers::TimesFour, Instance::offsetOfOriginalArrayStructureIDs()), scratch);
        needsOperations.append(jit.branch32(CCallHelpers::NotEqual, scratch, Address(scratch2, JSCell::structureIDOffset())));
        jit.loadPtr(Address(scratch2, JSObject::butterflyOffset()), scratch);
        jit.load32(Address(scratch, Butterfly::offsetOfPublicLength()), scratch);
        jit.add64(scratch, lengthOrDestination);
    }, [&] {
        jit.add64(Address(cursor), lengthOrDestination);
    });
    needsOperations.append(jit.branch64(CCallHelpers::Above, lengthOrDestination, TrustedImm32(maxArguments)));
    jit.store64(lengthOrDestination, local(offsetOfLength));

    static_assert(stackAlignmentRegisters() == 2);
    jit.add32(TrustedImm32(1), lengthOrDestination, scratch);
    jit.and32(TrustedImm32(~1), scratch);
    jit.lshiftPtr(TrustedImm32(3), scratch);
    jit.subPtr(CCallHelpers::stackPointerRegister, scratch, scratch);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfVM()), scratch2);
    overflow.append(jit.branchPtr(CCallHelpers::Above, Address(scratch2, VM::offsetOfSoftStackLimit()), scratch));
    overflow.append(jit.branchPtr(CCallHelpers::Above, scratch, GPRInfo::callFrameRegister));
    jit.move(scratch, CCallHelpers::stackPointerRegister);

    auto copy = [&](bool mayHaveHoles) {
        Jump none = jit.branchTest64(CCallHelpers::Zero, scratch);
        CCallHelpers::Label next = jit.label();
        jit.load64(Address(scratch2), scratch3);
        if (mayHaveHoles) {
            Jump isNotHole = jit.branchTest64(CCallHelpers::NonZero, scratch3);
            jit.move(CCallHelpers::TrustedImm64(JSValue::ValueUndefined), scratch3);
            isNotHole.link(&jit);
        }
        jit.store64(scratch3, Address(lengthOrDestination));
        jit.addPtr(TrustedImm32(sizeof(EncodedJSValue)), scratch2);
        jit.addPtr(TrustedImm32(sizeof(EncodedJSValue)), lengthOrDestination);
        jit.sub64(TrustedImm32(1), scratch);
        jit.branchTest64(CCallHelpers::NonZero, scratch).linkTo(next, &jit);
        none.link(&jit);
    };
    jit.move(CCallHelpers::stackPointerRegister, lengthOrDestination);
    forEachItem([&] {
        jit.load64(Address(cursor), scratch);
        jit.store64(scratch, Address(lengthOrDestination));
        jit.addPtr(TrustedImm32(sizeof(EncodedJSValue)), lengthOrDestination);
    }, [&] {
        jit.load64(Address(cursor), scratch2);
        jit.loadPtr(Address(scratch2, JSObject::butterflyOffset()), scratch2);
        jit.load32(Address(scratch2, Butterfly::offsetOfPublicLength()), scratch);
        copy(true);
    }, [&] {
        jit.load64(Address(cursor), scratch);
        jit.loadPtr(Address(cursor, sizeof(EncodedJSValue)), scratch2);
        copy(false);
    });
    Jump argumentsAreLoaded = jit.jump();

    needsOperations.link(&jit);
    jit.move(argumentGPR(1), A2);
    jit.move(argumentGPR(0), A1);
    callOperation(Entry::operationAOTSizeOfVarargs);
    exception.append(jit.branchTestPtr(CCallHelpers::NonZero, GPRInfo::returnValueGPR2));
    jit.store64(GPRInfo::returnValueGPR, local(offsetOfLength));

    jit.add32(TrustedImm32(1), GPRInfo::returnValueGPR, T11);
    jit.and32(TrustedImm32(~1), T11);
    jit.lshiftPtr(TrustedImm32(3), T11);
    jit.subPtr(CCallHelpers::stackPointerRegister, T11, T11);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfVM()), T13);
    overflow.append(jit.branchPtr(CCallHelpers::Above, Address(T13, VM::offsetOfSoftStackLimit()), T11));
    overflow.append(jit.branchPtr(CCallHelpers::Above, T11, GPRInfo::callFrameRegister));
    jit.move(T11, CCallHelpers::stackPointerRegister);

    jit.move(CCallHelpers::stackPointerRegister, A1);
    jit.load64(local(offsetOfList), A2);
    jit.load64(local(offsetOfFirst), A3);
    jit.load64(local(offsetOfLength), A4);
    callOperation(Entry::operationAOTLoadVarargs);
    exception.append(jit.branchTestPtr(CCallHelpers::NonZero, GPRInfo::returnValueGPR));

    argumentsAreLoaded.link(&jit);
    jit.load64(local(offsetOfCallee), calleeGPR);
    jit.load64(local(offsetOfThis), thisGPR);
    jit.load64(local(offsetOfLength), argumentGPR(0));
    jit.move(CCallHelpers::stackPointerRegister, argumentGPR(1));
    if (inTailPosition)
        callListInTailPosition(jit);
    else {
        if (kind == CodeSpecializationKind::CodeForCall) {
            callVarargsReturnAddress() = jit.label();
#if CPU(ARM64)
            loadLabelAddress(jit, stubLabels()[static_cast<unsigned>(Stub::CallList)], callTargetGPR);
            jit.jump().linkTo(callTargetWithList(), &jit);
#else
            loadLabelAddress(jit, returnFromCallWithList(), T11);
            jit.push(T11);
            s_callsBetweenStubs->append({ jit.nearTailCall(), Stub::CallList });
#endif
        } else {
            callStubFromStub(jit, kind == CodeSpecializationKind::CodeForCall ? Stub::CallList : Stub::ConstructList);
            jit.emitFunctionEpilogue();
            jit.ret();
        }
    }

    exception.link(&jit);
    jit.emitFunctionEpilogue();
    generateHandleException(jit);

    overflow.link(&jit);
    jit.emitFunctionEpilogue();
    throwStackOverflow(jit);
}

static void generateCallVarargs(CCallHelpers& jit) { generateCallVarargsTo(jit, CodeSpecializationKind::CodeForCall); }
static void generateConstructVarargs(CCallHelpers& jit) { generateCallVarargsTo(jit, CodeSpecializationKind::CodeForConstruct); }
static void generateTailCallVarargs(CCallHelpers& jit) { generateCallVarargsTo(jit, CodeSpecializationKind::CodeForCall, true); }
static void generateTailCallList(CCallHelpers& jit)
{
    jit.emitFunctionPrologue();
    callListInTailPosition(jit);
}
static void generateCallList(CCallHelpers& jit) { generateCallListTo(jit, CodeSpecializationKind::CodeForCall); }
static void generateConstructList(CCallHelpers& jit) { generateCallListTo(jit, CodeSpecializationKind::CodeForConstruct); }
static void generateCall(CCallHelpers& jit) { generateCallTo(jit, CodeSpecializationKind::CodeForCall, std::nullopt); }
static void generateConstruct(CCallHelpers& jit) { generateCallTo(jit, CodeSpecializationKind::CodeForConstruct, std::nullopt); }
static void generateCallCached(CCallHelpers& jit) { jit.breakpoint(); }
static void generateCallValue(CCallHelpers& jit) { jit.breakpoint(); }
static void generateCallMethod(CCallHelpers& jit) { jit.breakpoint(); }
static void generateCallMethodRead(CCallHelpers& jit) { jit.breakpoint(); }

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

static void getWellKnownInStubFrame(CCallHelpers& jit, WellKnownIdentifier identifier, CCallHelpers::JumpList& exception)
{
    jit.load64(slotWord(A1, 0), T11);
    Jump hasDifferentStructure = branchIfStructureDiffers(jit, R0, T11);
    Jump isIndirect = branchIfSlotHas<Slot::isIndirect>(jit, T11);
    loadDirectLocation(jit, A1, T11, T11);
    jit.load64(CCallHelpers::BaseIndex(R0, T11, CCallHelpers::TimesEight), R0);
    Jump found = jit.jump();

    hasDifferentStructure.link(&jit);
    isIndirect.link(&jit);
    jit.move(A1, A3);
    jit.move(R0, A1);
    jit.move(TrustedImm32(static_cast<uint32_t>(identifier)), A2);
    loadInstance(jit, T11);
    jit.move(T11, A0);
    jit.loadPtr(Address(T11, Instance::offsetOfRuntimeTable()), T11);
    jit.loadPtr(Address(T11, static_cast<unsigned>(Entry::operationAOTGetByIdWellKnown) * sizeof(void*)), T11);
    jit.call(T11, OperationPtrTag);
    exception.append(jit.branchTestPtr(CCallHelpers::NonZero, GPRInfo::returnValueGPR2));
    found.link(&jit);
}

static void callInStubFrameForObject(CCallHelpers& jit, CCallHelpers::JumpList& notObject)
{
    jit.move(TrustedImm32(0), countGPR);
    callStubFromStub(jit, Stub::Call);
    notObject.append(jit.branchIfNotCell(R0));
    notObject.append(jit.branchIfNotObject(R0));
}

static void throwFromStubFrame(CCallHelpers& jit, CCallHelpers::JumpList& notObject, CCallHelpers::JumpList& exception)
{
    notObject.link(&jit);
    loadInstance(jit, T11);
    jit.move(T11, A0);
    jit.loadPtr(Address(T11, Instance::offsetOfRuntimeTable()), T11);
    jit.loadPtr(Address(T11, static_cast<unsigned>(Entry::operationAOTThrowIteratorResultIsNotObject) * sizeof(void*)), T11);
    jit.call(T11, OperationPtrTag);
    exception.link(&jit);
    jit.emitFunctionEpilogue();
    loadInstance(jit, T9);
    jumpToEntry(jit, T9, Entry::HandleException);
}

template<typename Iterator, typename Collection>
static void advanceMapOrSetIterator(CCallHelpers& jit, JSType iteratorType)
{
    using Helper = typename Collection::Helper;
    using Field = typename Iterator::Field;
    constexpr GPRReg data = T11;
    constexpr GPRReg entry = T12;
    constexpr GPRReg index = T9;
    constexpr GPRReg key = A3;
    constexpr GPRReg scratch = A4;
    static_assert(noOverlap(R0, A1, data, entry, index, key, scratch));
    static_assert(Helper::EntrySize == 2 || Helper::EntrySize == 3);
    static_assert(Helper::bucketCount(16) == 16);
    auto field = [](Field which) { return Address(A1, Iterator::offsetOfInternalField(static_cast<unsigned>(which))); };

    CCallHelpers::JumpList otherwise;
    otherwise.append(jit.branchIfNotType(A1, iteratorType));
    jit.load64(field(Field::Storage), data);
    otherwise.append(jit.branchTest64(CCallHelpers::Zero, data));
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfVM()), scratch);
    otherwise.append(jit.branchPtr(CCallHelpers::Equal, data, Address(scratch, VM::offsetOfOrderedHashTableSentinel())));
    jit.addPtr(TrustedImm32(JSCellButterfly::offsetOfData()), data);
    jit.load64(Address(data, Helper::aliveEntryCountIndex() * sizeof(EncodedJSValue)), key);
    otherwise.append(jit.branchIfNotInt32(key));
    jit.load32(field(Field::Entry), entry);
    jit.load32(Address(data, Helper::capacityIndex() * sizeof(EncodedJSValue)), index);
    jit.add32(TrustedImm32(Helper::hashTableStartIndex()), index);
    for (unsigned i = 0; i < Helper::EntrySize; ++i)
        jit.add32(entry, index);
    jit.load64(CCallHelpers::BaseIndex(data, index, CCallHelpers::TimesEight), key);
    Jump atEnd = jit.branchTest64(CCallHelpers::Zero, key);
    otherwise.append(jit.branch64(CCallHelpers::Equal, key, Address(scratch, VM::offsetOfOrderedHashTableDeletedValue())));
    auto advanceAndReturn = [&] {
        jit.add32(TrustedImm32(1), entry);
        jit.or64(numberTag, entry);
        jit.store64(entry, field(Field::Entry));
        jit.move(R0, A2);
        jit.move(key, A1);
        jit.move(TrustedImm32(JSValue::ValueFalse), R0);
        jit.ret();
    };
    jit.load32(field(Field::Kind), scratch);
    Jump wantsEntry = jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(static_cast<int32_t>(IterationKind::Entries)));
    if constexpr (Helper::EntrySize == 3) {
        Jump wantsKey = jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(static_cast<int32_t>(IterationKind::Keys)));
        jit.load64(CCallHelpers::BaseIndex(data, index, CCallHelpers::TimesEight, sizeof(EncodedJSValue)), key);
        wantsKey.link(&jit);
    }
    advanceAndReturn();

    wantsEntry.link(&jit);
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(6 * sizeof(void*)), sp);
    jit.store64(key, Address(sp, 0));
    if constexpr (Helper::EntrySize == 3)
        jit.load64(CCallHelpers::BaseIndex(data, index, CCallHelpers::TimesEight, sizeof(EncodedJSValue)), key);
    jit.store64(key, Address(sp, 8));
    jit.storePair64(R0, A1, Address(sp, 16));
    jit.store64(entry, Address(sp, 32));
    jit.move(sp, A0);
    jit.move(TrustedImm32(2), A1);
    callStubFromStub(jit, Stub::HelperNewArray);
    jit.move(GPRInfo::returnValueGPR, key);
    jit.loadPair64(Address(sp, 16), R0, A1);
    jit.load64(Address(sp, 32), entry);
    jit.emitFunctionEpilogue();
    otherwise.append(jit.branchTest64(CCallHelpers::Zero, key));
    advanceAndReturn();

    atEnd.link(&jit);
    jit.loadPtr(Address(scratch, VM::offsetOfOrderedHashTableSentinel()), scratch);
    jit.store64(scratch, field(Field::Storage));
    jit.move(R0, A2);
    jit.move(TrustedImm32(0), A1);
    jit.move(TrustedImm32(JSValue::ValueTrue), R0);
    jit.ret();

    otherwise.link(&jit);
}

static void advanceArrayIterator(CCallHelpers& jit)
{
    using Field = JSArrayIterator::Field;
    constexpr GPRReg storage = T11;
    constexpr GPRReg index = T12;
    constexpr GPRReg element = A3;
    constexpr GPRReg scratch = A4;
    static_assert(noOverlap(R0, A1, storage, index, element, scratch));
    auto field = [](Field which) { return Address(A1, JSArrayIterator::offsetOfInternalField(static_cast<unsigned>(which))); };

    CCallHelpers::JumpList otherwise;
    otherwise.append(jit.branchIfNotType(A1, JSArrayIteratorType));
    jit.load64(field(Field::Index), index);
    otherwise.append(jit.branchIfNotInt32(index));
    jit.load64(field(Field::IteratedObject), storage);
    jit.load8(Address(storage, JSCell::indexingTypeAndMiscOffset()), scratch);
    jit.and32(TrustedImm32(IsArray | IndexingShapeMask), scratch);
    Jump isInt32Shape = jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(ArrayWithInt32));
    otherwise.append(jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(ArrayWithContiguous)));
    isInt32Shape.link(&jit);
    jit.loadPtr(Address(storage, JSObject::butterflyOffset()), storage);
    jit.zeroExtend32ToWord(index, index);
    static_assert(JSArrayIterator::doneIndex == -1);
    Jump atEnd = jit.branch32(CCallHelpers::AboveOrEqual, index, Address(storage, Butterfly::offsetOfPublicLength()));
    jit.load64(CCallHelpers::BaseIndex(storage, index, CCallHelpers::TimesEight), element);
    otherwise.append(jit.branchTest64(CCallHelpers::Zero, element));
    auto advanceAndReturn = [&] {
        jit.add32(TrustedImm32(1), index);
        jit.or64(numberTag, index);
        jit.store64(index, field(Field::Index));
        jit.move(R0, A2);
        jit.move(element, A1);
        jit.move(TrustedImm32(JSValue::ValueFalse), R0);
        jit.ret();
    };
    jit.load32(field(Field::Kind), scratch);
    Jump wantsEntry = jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(static_cast<int32_t>(IterationKind::Entries)));
    Jump wantsValue = jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(static_cast<int32_t>(IterationKind::Values)));
    jit.or64(numberTag, index, element);
    wantsValue.link(&jit);
    advanceAndReturn();

    wantsEntry.link(&jit);
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(6 * sizeof(void*)), sp);
    jit.or64(numberTag, index, scratch);
    jit.storePair64(scratch, element, Address(sp, 0));
    jit.storePair64(R0, A1, Address(sp, 16));
    jit.store64(index, Address(sp, 32));
    jit.move(sp, A0);
    jit.move(TrustedImm32(2), A1);
    callStubFromStub(jit, Stub::HelperNewArray);
    jit.move(GPRInfo::returnValueGPR, element);
    jit.loadPair64(Address(sp, 16), R0, A1);
    jit.load64(Address(sp, 32), index);
    jit.emitFunctionEpilogue();
    otherwise.append(jit.branchTest64(CCallHelpers::Zero, element));
    advanceAndReturn();

    atEnd.link(&jit);
    jit.move(CCallHelpers::TrustedImm64(JSValue::encode(jsNumber(JSArrayIterator::doneIndex))), scratch);
    jit.store64(scratch, field(Field::Index));
    jit.move(R0, A2);
    jit.move(TrustedImm32(0), A1);
    jit.move(TrustedImm32(JSValue::ValueTrue), R0);
    jit.ret();

    otherwise.link(&jit);
}

static void advanceStringIterator(CCallHelpers& jit)
{
    using Field = JSStringIterator::Field;
    constexpr GPRReg impl = T11;
    constexpr GPRReg index = T12;
    constexpr GPRReg character = A3;
    constexpr GPRReg scratch = A4;
    static_assert(noOverlap(R0, A1, impl, index, character, scratch));
    auto field = [](Field which) { return Address(A1, JSStringIterator::offsetOfInternalField(static_cast<unsigned>(which))); };

    CCallHelpers::JumpList otherwise;
    otherwise.append(jit.branchIfNotType(A1, JSStringIteratorType));
    jit.load64(field(Field::IteratedString), impl);
    jit.loadPtr(Address(impl, JSString::offsetOfValue()), impl);
    otherwise.append(jit.branchTestPtr(CCallHelpers::NonZero, impl, TrustedImm32(JSString::isRopeInPointer)));
    static_assert(JSStringIterator::doneIndex == -1);
    jit.load32(field(Field::Index), index);
    Jump atEnd = jit.branch32(CCallHelpers::AboveOrEqual, index, Address(impl, StringImpl::lengthMemoryOffset()));
    jit.loadPtr(Address(impl, StringImpl::dataOffset()), scratch);
    Jump is16Bit = jit.branchTest32(CCallHelpers::Zero, Address(impl, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit()));
    jit.load8(CCallHelpers::BaseIndex(scratch, index, CCallHelpers::TimesOne), character);
    Jump characterReady = jit.jump();
    is16Bit.link(&jit);
    jit.load16(CCallHelpers::BaseIndex(scratch, index, CCallHelpers::TimesTwo), character);
    characterReady.link(&jit);
    static_assert(maxSingleCharacterString < 0xd800);
    otherwise.append(jit.branch32(CCallHelpers::Above, character, TrustedImm32(maxSingleCharacterString)));
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfVM()), scratch);
    jit.addPtr(TrustedImm32(OBJECT_OFFSETOF(VM, smallStrings) + SmallStrings::offsetOfSingleCharacterStrings()), scratch);
    jit.loadPtr(CCallHelpers::BaseIndex(scratch, character, CCallHelpers::TimesEight), character);
    otherwise.append(jit.branchTestPtr(CCallHelpers::Zero, character));
    jit.add32(TrustedImm32(1), index);
    jit.or64(numberTag, index);
    jit.store64(index, field(Field::Index));
    jit.move(R0, A2);
    jit.move(character, A1);
    jit.move(TrustedImm32(JSValue::ValueFalse), R0);
    jit.ret();

    atEnd.link(&jit);
    jit.move(CCallHelpers::TrustedImm64(JSValue::encode(jsNumber(JSStringIterator::doneIndex))), scratch);
    jit.store64(scratch, field(Field::Index));
    jit.move(R0, A2);
    jit.move(TrustedImm32(0), A1);
    jit.move(TrustedImm32(JSValue::ValueTrue), R0);
    jit.ret();

    otherwise.link(&jit);
}

static void generateIteratorNext(CCallHelpers& jit)
{
    CCallHelpers::JumpList generic;
    CCallHelpers::JumpList indexSlow;
    auto handled = [&] {
        jit.ret();
    };
    auto doneIfEmpty = [&](GPRReg value) {
        static_assert(JSValue::ValueTrue == JSValue::ValueFalse + 1);
        jit.compare64(CCallHelpers::Equal, value, TrustedImm32(0), R0);
        jit.add64(TrustedImm32(JSValue::ValueFalse), R0);
    };
    auto callKeeping = [&](Entry operation, GPRReg kept, const auto& setUp) {
        jit.emitFunctionPrologue();
        jit.subPtr(TrustedImm32(16), CCallHelpers::stackPointerRegister);
        jit.store64(kept, Address(CCallHelpers::stackPointerRegister));
        loadInstance(jit, T11);
        setUp();
        jit.move(T11, A0);
        jit.loadPtr(Address(T11, Instance::offsetOfRuntimeTable()), T11);
        jit.loadPtr(Address(T11, static_cast<unsigned>(operation) * sizeof(void*)), T11);
        jit.call(T11, OperationPtrTag);
#if CPU(X86_64)
        static_assert(GPRInfo::returnValueGPR2 == A2);
        jit.move(GPRInfo::returnValueGPR2, T11);
        jit.load64(Address(CCallHelpers::stackPointerRegister), A2);
        jit.emitFunctionEpilogue();
        Jump exception = jit.branchTestPtr(CCallHelpers::NonZero, T11);
#else
        jit.load64(Address(CCallHelpers::stackPointerRegister), A2);
        jit.emitFunctionEpilogue();
        Jump exception = jit.branchTestPtr(CCallHelpers::NonZero, GPRInfo::returnValueGPR2);
#endif
        jit.move(GPRInfo::returnValueGPR, A1);
        doneIfEmpty(A1);
        handled();
        exception.link(&jit);
        loadInstance(jit, T9);
        jumpToEntry(jit, T9, Entry::HandleException);
    };

    Jump nextIsCell = jit.branchIfCell(R0);

    generic.append(jit.branchIfNotCell(A1));
    generic.append(jit.branchIfNotType(A1, SentinelType));

    indexSlow.append(jit.branch64(CCallHelpers::Below, R0, numberTag));
    indexSlow.append(jit.branchIfNotCell(A2));
    indexSlow.append(jit.branchIfNotType(A2, ArrayType));
    jit.load8(Address(A2, JSCell::indexingTypeAndMiscOffset()), T11);
    jit.and32(TrustedImm32(IndexingShapeMask), T11);
    CCallHelpers::JumpList atEndCase;
    Jump isInt32Shape = jit.branch32(CCallHelpers::Equal, T11, TrustedImm32(Int32Shape));
    Jump isDoubleShape = jit.branch32(CCallHelpers::Equal, T11, TrustedImm32(DoubleShape));
    indexSlow.append(jit.branch32(CCallHelpers::NotEqual, T11, TrustedImm32(ContiguousShape)));
    isInt32Shape.link(&jit);
    jit.loadPtr(Address(A2, JSObject::butterflyOffset()), T11);
    jit.load32(Address(T11, Butterfly::offsetOfPublicLength()), T12);
    atEndCase.append(jit.branch32(CCallHelpers::AboveOrEqual, R0, T12));
    indexSlow.append(jit.branch32(CCallHelpers::Equal, R0, TrustedImm32(std::numeric_limits<int32_t>::max())));
    jit.zeroExtend32ToWord(R0, T12);
    jit.load64(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesEight), T11);
    indexSlow.append(jit.branchTest64(CCallHelpers::Zero, T11));
    jit.move(T11, A1);
    jit.add32(TrustedImm32(1), T12);
    jit.add64(numberTag, T12, A2);
    jit.move(TrustedImm32(JSValue::ValueFalse), R0);
    handled();

    isDoubleShape.link(&jit);
    jit.loadPtr(Address(A2, JSObject::butterflyOffset()), T11);
    atEndCase.append(jit.branch32(CCallHelpers::AboveOrEqual, R0, Address(T11, Butterfly::offsetOfPublicLength())));
    indexSlow.append(jit.branch32(CCallHelpers::Equal, R0, TrustedImm32(std::numeric_limits<int32_t>::max())));
    jit.zeroExtend32ToWord(R0, T12);
    jit.loadDouble(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesEight), FPRInfo::fpRegT0);
    indexSlow.append(jit.branchIfNaN(FPRInfo::fpRegT0));
    {
        CCallHelpers::JumpList isNoInt32;
        jit.branchConvertDoubleToInt32(FPRInfo::fpRegT0, A1, isNoInt32, FPRInfo::fpRegT1);
        jit.or64(numberTag, A1);
        Jump isBoxed = jit.jump();
        isNoInt32.link(&jit);
        jit.boxDouble(FPRInfo::fpRegT0, A1);
        isBoxed.link(&jit);
    }
    jit.add32(TrustedImm32(1), T12);
    jit.add64(numberTag, T12, A2);
    jit.move(TrustedImm32(JSValue::ValueFalse), R0);
    handled();

    atEndCase.link(&jit);
    jit.move(CCallHelpers::TrustedImm64(JSValue::encode(jsNumber(JSArrayIterator::doneIndex))), A2);
    jit.move(TrustedImm32(0), A1);
    jit.move(TrustedImm32(JSValue::ValueTrue), R0);
    handled();

    indexSlow.link(&jit);
    callKeeping(Entry::operationAOTIteratorNextWithIndex, R0, [&] {
        jit.move(A2, A1);
        jit.move(CCallHelpers::stackPointerRegister, A2);
    });

    nextIsCell.link(&jit);
    generic.append(jit.branchIfNotType(R0, SentinelType));
    advanceMapOrSetIterator<JSSetIterator, JSSet>(jit, JSSetIteratorType);
    advanceMapOrSetIterator<JSMapIterator, JSMap>(jit, JSMapIteratorType);
    advanceArrayIterator(jit);
    advanceStringIterator(jit);
    callKeeping(Entry::operationAOTIteratorNextTryFast, R0, [&] { });

    generic.link(&jit);
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    constexpr int32_t savedNext = 0;
    constexpr int32_t savedSlots = 8;
    constexpr int32_t savedResult = 16;
    CCallHelpers::JumpList notObject;
    CCallHelpers::JumpList exception;
    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(32), sp);
    jit.store64(R0, Address(sp, savedNext));
    jit.storePtr(A3, Address(sp, savedSlots));
    jit.move(R0, calleeGPR);
    jit.move(A1, thisGPR);
    callInStubFrameForObject(jit, notObject);
    jit.store64(R0, Address(sp, savedResult));
    jit.loadPtr(Address(sp, savedSlots), A1);
    getWellKnownInStubFrame(jit, WellKnownIdentifier::Done, exception);
    callStubFromStub(jit, Stub::ToBoolean);
    Jump isDone = jit.branchTest32(CCallHelpers::NonZero, R0);
    jit.load64(Address(sp, savedResult), R0);
    jit.loadPtr(Address(sp, savedSlots), A1);
    jit.addPtr(TrustedImm32(sizeof(Slot)), A1);
    getWellKnownInStubFrame(jit, WellKnownIdentifier::Value, exception);
    jit.move(R0, A1);
    jit.move(TrustedImm32(JSValue::ValueFalse), R0);
    Jump hasValue = jit.jump();
    isDone.link(&jit);
    jit.move(CCallHelpers::TrustedImm64(JSValue::ValueUndefined), A1);
    jit.move(TrustedImm32(JSValue::ValueTrue), R0);
    hasValue.link(&jit);
    jit.load64(Address(sp, savedNext), A2);
    jit.emitFunctionEpilogue();
    jit.ret();

    throwFromStubFrame(jit, notObject, exception);
}

static void generateIteratorOpen(CCallHelpers& jit)
{
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    {
        Jump isNotCell = jit.branchIfNotCell(R0);
        jit.load8(Address(R0, JSCell::indexingTypeAndMiscOffset()), T11);
        jit.urshift32(TrustedImm32(Instance::arrayKindShift), T11);
        jit.and32(TrustedImm32(Instance::numberOfArrayKinds - 1), T11);
        jit.load32(CCallHelpers::BaseIndex(instanceGPR, T11, CCallHelpers::TimesFour, Instance::offsetOfOriginalArrayStructureIDs()), T11);
        jit.load32(Address(R0, JSCell::structureIDOffset()), T13);
        Jump isOriginalArray = jit.branch32(CCallHelpers::Equal, T11, T13);
        Jump isRegExpMatchesArray = jit.branch32(CCallHelpers::Equal, T13, Address(instanceGPR, Instance::offsetOfRegExpMatchesArrayStructureIDs()));
        Jump isOtherKind = jit.branch32(CCallHelpers::NotEqual, T13, Address(instanceGPR, Instance::offsetOfRegExpMatchesArrayStructureIDs() + sizeof(uint32_t)));
        isOriginalArray.link(&jit);
        isRegExpMatchesArray.link(&jit);
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfArrayIterationSentinel()), R0);
        jit.move(CCallHelpers::TrustedImm64(JSValue::encode(jsNumber(0))), A1);
        jit.ret();
        isNotCell.link(&jit);
        isOtherKind.link(&jit);
    }

    constexpr int32_t savedNext = 0;
    constexpr int32_t savedSlot = 8;
    constexpr int32_t savedIterable = 16;
    constexpr int32_t savedSymbolIterator = 24;
    CCallHelpers::JumpList notObject;
    CCallHelpers::JumpList exception;
    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(32), sp);
    jit.storePtr(A2, Address(sp, savedSlot));
    jit.store64(R0, Address(sp, savedIterable));
    jit.store64(A1, Address(sp, savedSymbolIterator));
    jit.move(A1, A2);
    jit.move(R0, A1);
    jit.addPtr(TrustedImm32(savedNext), sp, A3);
    loadInstance(jit, T11);
    jit.move(T11, A0);
    jit.loadPtr(Address(T11, Instance::offsetOfRuntimeTable()), T11);
    jit.loadPtr(Address(T11, static_cast<unsigned>(Entry::operationAOTIteratorOpenTryFast) * sizeof(void*)), T11);
    jit.call(T11, OperationPtrTag);
    exception.append(jit.branchTestPtr(CCallHelpers::NonZero, GPRInfo::returnValueGPR2));
    Jump isGeneric = jit.branchTest64(CCallHelpers::Zero, R0);
    jit.load64(Address(sp, savedNext), A1);
    jit.emitFunctionEpilogue();
    jit.ret();

    isGeneric.link(&jit);
    jit.load64(Address(sp, savedIterable), thisGPR);
    jit.load64(Address(sp, savedSymbolIterator), calleeGPR);
    callInStubFrameForObject(jit, notObject);
    jit.store64(R0, Address(sp, savedIterable));
    jit.loadPtr(Address(sp, savedSlot), A1);
    getWellKnownInStubFrame(jit, WellKnownIdentifier::Next, exception);
    jit.move(R0, A1);
    jit.load64(Address(sp, savedIterable), R0);
    jit.emitFunctionEpilogue();
    jit.ret();

    throwFromStubFrame(jit, notObject, exception);
}

static void generateIteratorCloseCheck(CCallHelpers& jit)
{
    Jump isNotCell = jit.branchIfNotCell(R0);
    Jump isNotMarked = jit.branchIfNotType(R0, SentinelType);
    loadInstance(jit, T11);
    jit.loadPtr(Address(T11, Instance::offsetOfGlobalObject()), T12);
    jit.loadPtr(Address(T12, JSGlobalObject::offsetOfArrayIteratorProtocolWatchpointSet() + InlineWatchpointSet::offsetOfData()), T12);
    Jump isInvalidated = jit.branchPtr(CCallHelpers::Equal, T12, CCallHelpers::TrustedImmPtr(InlineWatchpointSet::encodeState(IsInvalidated)));
    Jump isThin = jit.branchTestPtr(CCallHelpers::NonZero, T12, TrustedImm32(InlineWatchpointSet::IsThinFlag));
    jit.load8(Address(T12, WatchpointSet::offsetOfState()), T12);
    Jump isAlsoInvalidated = jit.branch32(CCallHelpers::Equal, T12, TrustedImm32(IsInvalidated));
    isThin.link(&jit);
    isNotCell.link(&jit);
    isNotMarked.link(&jit);
    jit.ret();

    isInvalidated.link(&jit);
    isAlsoInvalidated.link(&jit);
    jit.move(T11, A0);
    jit.loadPtr(Address(T11, Instance::offsetOfRuntimeTable()), T11);
    jit.loadPtr(Address(T11, static_cast<unsigned>(Entry::operationAOTMaterializeArrayIterator) * sizeof(void*)), T11);
    callAndCheckException(jit, T11, Returns::Value);
}

static void generateCallIntrinsic(CCallHelpers& jit) { jit.breakpoint(); }

static unsigned argumentCountOf(StubIntrinsic intrinsic)
{
    switch (intrinsic) {
#define AOT_STUB_INTRINSIC_ARGUMENTS(name, text, argumentCount, result) \
    case StubIntrinsic::name: \
        return argumentCount;
    FOR_EACH_AOT_STUB_INTRINSIC(AOT_STUB_INTRINSIC_ARGUMENTS)
#undef AOT_STUB_INTRINSIC_ARGUMENTS
    default:
        RELEASE_ASSERT_NOT_REACHED();
        return 0;
    }
}

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
static void findInCompactTable(CCallHelpers& jit, CCallHelpers::JumpList& otherwise, MissesInCompactTable& misses, CCallHelpers::JumpList& found)
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
        jit.lshift64(TrustedImm32(Typed::cell32Shift), candidate);
        jit.add64(Address(instanceGPR, Instance::offsetOfCellCageBase()), candidate);
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
    CCallHelpers::Label compare = jit.label();
    found.append(jit.branchTest32(CCallHelpers::Zero, position));
    jit.sub32(TrustedImm32(1), position);
    jit.load8(BaseIndex(candidate, position, CCallHelpers::TimesOne), tag);
    jit.load8(BaseIndex(scratch, position, CCallHelpers::TimesOne), mask);
    jit.branch32(CCallHelpers::Equal, tag, mask).linkTo(compare, &jit);
    otherwise.append(jit.jump());
}

// Without the last: where a Typed::CompactTable does not have the key, it is absent like elsewhere.
template<typename MapOrSet>
static void findInMapOrSet(CCallHelpers& jit, CCallHelpers::JumpList& otherwise, CCallHelpers::JumpList& absentCases, CCallHelpers::JumpList& foundInCompactTable, MissesInCompactTable* missesInCompactTable, CCallHelpers::JumpList* hasNoTable = nullptr)
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
    Jump isString = jit.branchIfType(key, StringType);
    otherwise.append(jit.branchIfType(key, HeapBigIntType));
    plain.append(jit.jump());
    isString.link(&jit);
    jit.loadPtr(Address(key, JSString::offsetOfValue()), keyImpl);
    otherwise.append(jit.branchTestPtr(CCallHelpers::NonZero, keyImpl, TrustedImm32(JSString::isRopeInPointer)));
    jit.load32(Address(keyImpl, StringImpl::flagsOffset()), hash);
    jit.urshift32(TrustedImm32(StringImpl::s_flagCount), hash);
    otherwise.append(jit.branchTest32(CCallHelpers::Zero, hash));
    hashed.append(jit.branchTestPtr(CCallHelpers::NonZero, data));
#if CPU(X86_64)
    jit.store32(hash, Address(Lookup::sp, Lookup::savedHash));
#endif
    Lookup::loadReceiver(jit, data);
    jit.loadPtr(Address(data, MapOrSet::offsetOfCompactTable()), data);
    // With the last: the collection has nothing yet, and the key is a string that is flat and hashed.
    (hasNoTable ? *hasNoTable : absentCases).append(jit.branchTestPtr(CCallHelpers::Zero, data));
    if (Options::useAOTWideReferences())
        otherwise.append(jit.jump());
    else if (missesInCompactTable)
        findInCompactTable(jit, otherwise, *missesInCompactTable, foundInCompactTable);
    else {
        MissesInCompactTable misses;
        findInCompactTable(jit, otherwise, misses, foundInCompactTable);
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
    CCallHelpers::Label compare = jit.label();
    found.append(jit.branchTest32(CCallHelpers::Zero, count));
    jit.sub32(TrustedImm32(1), count);
    jit.load8(CCallHelpers::BaseIndex(entryImpl, count, CCallHelpers::TimesOne), scratch2);
    jit.load8(CCallHelpers::BaseIndex(scratch, count, CCallHelpers::TimesOne), scratch3);
    jit.branch32(CCallHelpers::Equal, scratch2, scratch3).linkTo(compare, &jit);

    next.link(&jit);
    jit.load64(Address(slot, Helper::ChainOffset * sizeof(EncodedJSValue)), count);
    jit.jump().linkTo(loop, &jit);

    found.link(&jit);
}

enum class MapOrSetOperation : uint8_t { MapGet, MapHas, MapSet, SetHas, SetAdd, MapDelete, SetDelete };

static void generateMapOrSetOperation(CCallHelpers& jit, MapOrSetOperation operation, CCallHelpers::JumpList* needsHostFunction)
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
        giveUp.append(jit.branchTest64(CCallHelpers::NonZero, value, TrustedImm32((1 << Typed::cell32Shift) - 1)));
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
        jit.add32(TrustedImm32(1), entry);
        jit.store32(entry, Address(table, Table::offsetOfUsed()));
        barrierAndReturnReceiver();
    };

    // The first entry of a collection, where the value is a cell of a block, `undefined`, `null` or a boolean: the table is made here, as CompactTable::tryAdd() makes it. All else is absent like elsewhere.
    CCallHelpers::JumpList hasNoTable;
    bool makesTable = addsEntries && !Options::useAOTWideReferences();
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
            absentCases.append(jit.branchTest64(CCallHelpers::NonZero, value, TrustedImm32((1 << Typed::cell32Shift) - 1)));
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
        jit.store64(CCallHelpers::TrustedImm64(1), Address(table, Table::offsetOfUsed()));
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
    if (isOfSet)
        findInMapOrSet<JSSet>(jit, giveUp, absentCases, foundInCompactTable, addsEntries ? &missesInCompactTable : nullptr, makesTable ? &hasNoTable : nullptr);
    else
        findInMapOrSet<JSMap>(jit, giveUp, absentCases, foundInCompactTable, addsEntries ? &missesInCompactTable : nullptr, makesTable ? &hasNoTable : nullptr);
    switch (operation) {
    case MapOrSetOperation::MapGet: {
        jit.load64(Address(Lookup::slot, sizeof(EncodedJSValue)), result);
        Lookup::leave(jit);
        jit.ret();
        constexpr GPRReg values = Lookup::entryImpl;
        foundInCompactTable.link(&jit);
        jit.load32(Address(Lookup::data, Table::offsetOfCapacity()), Lookup::scratch);
        jit.getEffectiveAddress(CCallHelpers::BaseIndex(values, Lookup::scratch, CCallHelpers::TimesFour), values);
        Jump isWide = jit.branch8(CCallHelpers::NotEqual, Address(Lookup::data, Table::offsetOfValueWidth()), TrustedImm32(static_cast<int32_t>(Table::Width::Narrow)));
        jit.load32(CCallHelpers::BaseIndex(values, Lookup::slot, CCallHelpers::TimesFour), result);
        Jump isNoCell = jit.branch32(CCallHelpers::BelowOrEqual, result, TrustedImm32(Typed::undefinedCell32));
        jit.lshift64(TrustedImm32(Typed::cell32Shift), result);
        jit.add64(Address(instanceGPR, Instance::offsetOfCellCageBase()), result);
        isNoCell.link(&jit);
        Lookup::leave(jit);
        jit.ret();
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
        deferToOperation(Entry::operationAOTMapSet, true, true);
        giveUp.link(&jit);
        deferToOperation(Entry::operationAOTMapSetGeneric, true, false);
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
        jit.add32(TrustedImm32(1), Address(Lookup::data, Table::offsetOfDeleted()));
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
        deferToOperation(Entry::operationAOTSetAdd, false, true);
        giveUp.link(&jit);
        deferToOperation(Entry::operationAOTSetAddGeneric, false, false);
        break;
    }
    if (needsHostFunction) {
        isOfAnotherRealm.link(&jit);
        Lookup::leaveAsEntered(jit);
        needsHostFunction->append(jit.jump());
    }
}

static void generateMapGet(CCallHelpers& jit) { generateMapOrSetOperation(jit, MapOrSetOperation::MapGet, nullptr); }
static void generateMapHas(CCallHelpers& jit) { generateMapOrSetOperation(jit, MapOrSetOperation::MapHas, nullptr); }
static void generateMapSet(CCallHelpers& jit) { generateMapOrSetOperation(jit, MapOrSetOperation::MapSet, nullptr); }
static void generateSetHas(CCallHelpers& jit) { generateMapOrSetOperation(jit, MapOrSetOperation::SetHas, nullptr); }
static void generateSetAdd(CCallHelpers& jit) { generateMapOrSetOperation(jit, MapOrSetOperation::SetAdd, nullptr); }
static void generateMapDelete(CCallHelpers& jit) { generateMapOrSetOperation(jit, MapOrSetOperation::MapDelete, nullptr); }
static void generateSetDelete(CCallHelpers& jit) { generateMapOrSetOperation(jit, MapOrSetOperation::SetDelete, nullptr); }

enum class WeakLookup : uint8_t { Get, Has };

template<typename WeakMapOrSet>
static void generateWeakLookup(CCallHelpers& jit, WeakLookup lookup)
{
    using Bucket = typename WeakMapOrSet::BucketType;
    constexpr GPRReg collection = R0;
    constexpr GPRReg key = A1;
    constexpr GPRReg buffer = R0;
    constexpr GPRReg mask = T9;
    constexpr GPRReg index = T10;
    constexpr GPRReg entryKey = T11;
    constexpr unsigned wordsPerBucket = sizeof(Bucket) / sizeof(EncodedJSValue);
    static_assert(hasOneBitSet(wordsPerBucket) && wordsPerBucket * sizeof(EncodedJSValue) == sizeof(Bucket));
    static_assert(noOverlap(key, buffer, mask, index, entryKey));

    Jump isNotCell = jit.branchIfNotCell(key);
    jit.move(key, index);
    jit.rapidHashMix64(index, mask, entryKey);
    jit.load32(Address(collection, WeakMapOrSet::offsetOfCapacity()), mask);
    jit.loadPtr(Address(collection, WeakMapOrSet::offsetOfBuffer()), buffer);
    jit.sub32(TrustedImm32(1), mask);
    if (wordsPerBucket > 1) {
        jit.lshift64(TrustedImm32(getLSBSet(wordsPerBucket)), mask);
        jit.lshift64(TrustedImm32(getLSBSet(wordsPerBucket)), index);
    }
    CCallHelpers::Label next = jit.label();
    jit.and64(mask, index);
    jit.load64(CCallHelpers::BaseIndex(buffer, index, CCallHelpers::TimesEight, Bucket::offsetOfKey()), entryKey);
    Jump isFound = jit.branch64(CCallHelpers::Equal, entryKey, key);
    jit.add64(TrustedImm32(wordsPerBucket), index);
    jit.branchTest64(CCallHelpers::NonZero, entryKey).linkTo(next, &jit);

    isNotCell.link(&jit);
    if (lookup == WeakLookup::Get)
        jit.move(CCallHelpers::TrustedImm64(JSValue::ValueUndefined), R0);
    else
        jit.move(TrustedImm32(0), R0);
    jit.ret();

    isFound.link(&jit);
    if constexpr (WeakMapOrSet::isWeakMap()) {
        if (lookup == WeakLookup::Get) {
            jit.load64(CCallHelpers::BaseIndex(buffer, index, CCallHelpers::TimesEight, Bucket::offsetOfValue()), R0);
            jit.ret();
            return;
        }
    }
    jit.move(TrustedImm32(1), R0);
    jit.ret();
}

static void generateWeakMapGet(CCallHelpers& jit) { generateWeakLookup<JSWeakMap>(jit, WeakLookup::Get); }
static void generateWeakMapHas(CCallHelpers& jit) { generateWeakLookup<JSWeakMap>(jit, WeakLookup::Has); }
static void generateWeakSetHas(CCallHelpers& jit) { generateWeakLookup<JSWeakSet>(jit, WeakLookup::Has); }

enum class ArraySearch : uint8_t { Includes, IndexOf };

static void generateArraySearch(CCallHelpers& jit, ArraySearch search)
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
}

static void generateArrayIncludes(CCallHelpers& jit) { generateArraySearch(jit, ArraySearch::Includes); }
static void generateArrayIndexOf(CCallHelpers& jit) { generateArraySearch(jit, ArraySearch::IndexOf); }

static void generateChangeOfCase(CCallHelpers& jit, Entry operation, char firstLetterToChange)
{
    constexpr GPRReg string = A1;
    constexpr GPRReg index = A2;
    constexpr GPRReg distance = A3;
    constexpr GPRReg characters = T9;
    constexpr GPRReg length = T10;
    constexpr GPRReg character = T11;
    static_assert(noOverlap(R0, string, index, distance, characters, length, character));
    CCallHelpers::JumpList needsOperation;

    jit.move(TrustedImm32(0), index);
    jit.loadPtr(Address(string, JSString::offsetOfValue()), characters);
    needsOperation.append(jit.branchIfRopeStringImpl(characters));
    needsOperation.append(jit.branchTest32(CCallHelpers::Zero, Address(characters, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.load32(Address(characters, StringImpl::lengthMemoryOffset()), length);
    jit.loadPtr(Address(characters, StringImpl::dataOffset()), characters);
    Jump isEmpty = jit.branchTest32(CCallHelpers::Zero, length);
    CCallHelpers::Label next = jit.label();
    jit.load8(CCallHelpers::BaseIndex(characters, index, CCallHelpers::TimesOne), character);
    jit.sub32(character, TrustedImm32(firstLetterToChange), distance);
    needsOperation.append(jit.branch32(CCallHelpers::BelowOrEqual, distance, TrustedImm32('z' - 'a')));
    needsOperation.append(jit.branchTest32(CCallHelpers::NonZero, character, TrustedImm32(0x80)));
    jit.add32(TrustedImm32(1), index);
    jit.branch32(CCallHelpers::NotEqual, index, length).linkTo(next, &jit);
    isEmpty.link(&jit);
    jit.move(string, R0);
    jit.ret();

    needsOperation.link(&jit);
    jit.move(TrustedImm32(static_cast<unsigned>(operation) * sizeof(void*)), T9);
    jit.jump().linkTo(stubLabels()[static_cast<unsigned>(Stub::OperationValueWithGlobalObject)], &jit);
}

static void generateToLowerCase(CCallHelpers& jit) { generateChangeOfCase(jit, Entry::operationToLowerCase, 'A'); }
static void generateToUpperCase(CCallHelpers& jit) { generateChangeOfCase(jit, Entry::operationToUpperCase, 'a'); }

static void generateCallIntrinsic(CCallHelpers& jit, StubIntrinsic intrinsic, CCallHelpers::JumpList& hasAnotherReceiver, CCallHelpers::JumpList& otherwise, CCallHelpers::JumpList& needsHostFunction)
{
    using namespace IntrinsicRegisters;
    auto checkCallee = [&](Entry function) {
        otherwise.append(jit.branchIfNotCell(callee));
        otherwise.append(jit.branchIfNotType(callee, JSFunctionType));
        jit.loadPtr(Address(callee, JSFunction::offsetOfExecutableOrRareData()), scratch2);
        otherwise.append(jit.branchTestPtr(CCallHelpers::NonZero, scratch2, TrustedImm32(JSFunction::aotFunctionTag)));
        Jump hasExecutable = jit.branchTestPtr(CCallHelpers::Zero, scratch2, TrustedImm32(JSFunction::rareDataTag));
        jit.loadPtr(Address(scratch2, FunctionRareData::offsetOfExecutable() - JSFunction::rareDataTag), scratch2);
        hasExecutable.link(&jit);
        jit.loadPtr(Address(scratch2, NativeExecutable::offsetOfNativeFunctionFor(CodeSpecializationKind::CodeForCall)), scratch2);
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), scratch3);
        otherwise.append(jit.branchPtr(CCallHelpers::NotEqual, scratch2, Address(scratch3, static_cast<unsigned>(function) * sizeof(void*))));
    };
    auto checkReceiver = [&](JSType type) {
        hasAnotherReceiver.append(jit.branchIfNotCell(receiver));
        hasAnotherReceiver.append(jit.branchIfNotType(receiver, type));
    };
    auto returnInt32 = [&](GPRReg value) {
        jit.or64(GPRInfo::numberTagRegister, value, result);
        jit.ret();
    };
    auto returnConstant = [&](JSValue value) {
        jit.move(CCallHelpers::TrustedImm64(JSValue::encode(value)), result);
        jit.ret();
    };
    auto requireCalleeOfThisRealm = [&](GPRReg function, GPRReg scratch, CCallHelpers::JumpList& isOfAnotherRealm) {
        jit.loadPtr(Address(function, JSCallee::offsetOfScopeChain()), scratch);
        isOfAnotherRealm.append(jit.branchPtr(CCallHelpers::NotEqual, scratch, Address(instanceGPR, Instance::offsetOfGlobalObject())));
    };
    auto jumpToOperation = [&](Stub stub, Entry operation) {
        jit.move(TrustedImm32(static_cast<unsigned>(operation) * sizeof(void*)), T9);
        jit.jump().linkTo(stubLabels()[static_cast<unsigned>(stub)], &jit);
    };
    auto noteEffects = [&] {
        jit.add32(TrustedImm32(1), Address(instanceGPR, Instance::offsetOfEffectEpoch()));
    };

    switch (intrinsic) {
    case StubIntrinsic::CharCodeAt:
    case StubIntrinsic::CodePointAt:
    case StubIntrinsic::CharAt: {
        constexpr GPRReg impl = scratch0;
        constexpr GPRReg index = scratch1;
        constexpr GPRReg characters = scratch2;
        constexpr GPRReg character = scratch3;
        checkReceiver(StringType);
        checkCallee(intrinsic == StubIntrinsic::CharCodeAt ? Entry::HostStringCharCodeAt : intrinsic == StubIntrinsic::CodePointAt ? Entry::HostStringCodePointAt : Entry::HostStringCharAt);
        needsHostFunction.append(jit.branchIfNotInt32(first));
        jit.loadPtr(Address(receiver, JSString::offsetOfValue()), impl);
        needsHostFunction.append(jit.branchTestPtr(CCallHelpers::NonZero, impl, TrustedImm32(JSString::isRopeInPointer)));
        jit.zeroExtend32ToWord(first, index);
        Jump isOutOfBounds = jit.branch32(CCallHelpers::AboveOrEqual, index, Address(impl, StringImpl::lengthMemoryOffset()));
        jit.loadPtr(Address(impl, StringImpl::dataOffset()), characters);
        Jump is16Bit = jit.branchTest32(CCallHelpers::Zero, Address(impl, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit()));
        jit.load8(CCallHelpers::BaseIndex(characters, index, CCallHelpers::TimesOne), character);
        Jump characterReady = jit.jump();
        is16Bit.link(&jit);
        jit.load16(CCallHelpers::BaseIndex(characters, index, CCallHelpers::TimesTwo), character);
        if (intrinsic == StubIntrinsic::CodePointAt) {
            jit.sub32(character, TrustedImm32(0xd800), impl);
            needsHostFunction.append(jit.branch32(CCallHelpers::Below, impl, TrustedImm32(0x800)));
        }
        characterReady.link(&jit);
        if (intrinsic != StubIntrinsic::CharAt) {
            returnInt32(character);
            isOutOfBounds.link(&jit);
            returnConstant(intrinsic == StubIntrinsic::CharCodeAt ? jsNaN() : jsUndefined());
            break;
        }
        needsHostFunction.append(jit.branch32(CCallHelpers::Above, character, TrustedImm32(maxSingleCharacterString)));
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfVM()), characters);
        jit.addPtr(TrustedImm32(OBJECT_OFFSETOF(VM, smallStrings) + SmallStrings::offsetOfSingleCharacterStrings()), characters);
        jit.loadPtr(CCallHelpers::BaseIndex(characters, character, CCallHelpers::TimesEight), characters);
        needsHostFunction.append(jit.branchTestPtr(CCallHelpers::Zero, characters));
        jit.move(characters, result);
        jit.ret();
        isOutOfBounds.link(&jit);
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfEmptyString()), result);
        jit.ret();
        break;
    }
    case StubIntrinsic::Push: {
        constexpr GPRReg butterfly = scratch0;
        constexpr GPRReg length = scratch1;
        CCallHelpers::JumpList slowCase;
        checkReceiver(ArrayType);
        checkCallee(Entry::HostArrayPush);
        jit.load8(Address(receiver, JSCell::indexingTypeAndMiscOffset()), scratch0);
        jit.and32(TrustedImm32(IndexingShapeMask | CopyOnWrite), scratch0);
        Jump contiguousCase = jit.branch32(CCallHelpers::Equal, scratch0, TrustedImm32(ContiguousShape));
        slowCase.append(jit.branch32(CCallHelpers::NotEqual, scratch0, TrustedImm32(Int32Shape)));
        slowCase.append(jit.branchIfNotInt32(first));
        contiguousCase.link(&jit);
        jit.loadPtr(Address(receiver, JSObject::butterflyOffset()), butterfly);
        jit.load32(Address(butterfly, Butterfly::offsetOfPublicLength()), length);
        slowCase.append(jit.branch32(CCallHelpers::AboveOrEqual, length, Address(butterfly, Butterfly::offsetOfVectorLength())));
        jit.store64(first, CCallHelpers::BaseIndex(butterfly, length, CCallHelpers::TimesEight));
        jit.add32(TrustedImm32(1), length);
        jit.store32(length, Address(butterfly, Butterfly::offsetOfPublicLength()));
        Jump notCell = jit.branchIfNotCell(first);
        jit.load8(Address(receiver, JSCell::cellStateOffset()), scratch2);
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfVM()), scratch3);
        Jump needsBarrier = jit.branch32(CCallHelpers::BelowOrEqual, scratch2, Address(scratch3, VM::offsetOfHeapBarrierThreshold()));
        notCell.link(&jit);
        returnInt32(length);

        needsBarrier.link(&jit);
        jit.move(receiver, T9);
        jit.or64(GPRInfo::numberTagRegister, length, result);
        jit.jump().linkTo(writeBarrierSlowCase(), &jit);

        slowCase.link(&jit);
        requireCalleeOfThisRealm(callee, scratch2, needsHostFunction);
        noteEffects();
        jit.move(first, A1);
        jit.move(receiver, A2);
        jumpToOperation(Stub::OperationValueWithGlobalObject, Entry::operationArrayPush);
        break;
    }
    case StubIntrinsic::Pop: {
        constexpr GPRReg butterfly = scratch0;
        constexpr GPRReg length = scratch1;
        CCallHelpers::JumpList slowCase;
        checkReceiver(ArrayType);
        checkCallee(Entry::HostArrayPop);
        jit.load8(Address(receiver, JSCell::indexingTypeAndMiscOffset()), scratch0);
        jit.and32(TrustedImm32(IndexingShapeMask | CopyOnWrite), scratch0);
        Jump contiguousCase = jit.branch32(CCallHelpers::Equal, scratch0, TrustedImm32(ContiguousShape));
        slowCase.append(jit.branch32(CCallHelpers::NotEqual, scratch0, TrustedImm32(Int32Shape)));
        contiguousCase.link(&jit);
        jit.loadPtr(Address(receiver, JSObject::butterflyOffset()), butterfly);
        jit.load32(Address(butterfly, Butterfly::offsetOfPublicLength()), length);
        Jump isEmpty = jit.branchTest32(CCallHelpers::Zero, length);
        jit.sub32(TrustedImm32(1), length);
        slowCase.append(jit.branch32(CCallHelpers::AboveOrEqual, length, Address(butterfly, Butterfly::offsetOfVectorLength())));
        jit.load64(CCallHelpers::BaseIndex(butterfly, length, CCallHelpers::TimesEight), scratch2);
        slowCase.append(jit.branchTest64(CCallHelpers::Zero, scratch2));
        jit.store64(TrustedImm32(0), CCallHelpers::BaseIndex(butterfly, length, CCallHelpers::TimesEight));
        jit.store32(length, Address(butterfly, Butterfly::offsetOfPublicLength()));
        jit.move(scratch2, result);
        jit.ret();

        isEmpty.link(&jit);
        returnConstant(jsUndefined());

        slowCase.link(&jit);
        requireCalleeOfThisRealm(callee, scratch2, needsHostFunction);
        noteEffects();
        jit.move(receiver, A1);
        jumpToOperation(Stub::OperationValueWithGlobalObject, Entry::operationArrayPop);
        break;
    }
    case StubIntrinsic::Slice:
    case StubIntrinsic::SliceWithEnd: {
        checkReceiver(StringType);
        checkCallee(Entry::HostStringSlice);
        needsHostFunction.append(jit.branchIfNotInt32(first));
        if (intrinsic == StubIntrinsic::SliceWithEnd)
            needsHostFunction.append(jit.branchIfNotInt32(second));
        requireCalleeOfThisRealm(callee, scratch2, needsHostFunction);
        static_assert(first == A0 && second == A1);
        if (intrinsic == StubIntrinsic::SliceWithEnd)
            jit.move(second, A3);
        else
            jit.move(TrustedImm32(std::numeric_limits<int32_t>::max()), A3);
        jit.move(first, A2);
        jit.move(receiver, A1);
        jumpToOperation(Stub::OperationValueWithGlobalObject, Entry::operationStringSliceWithEnd);
        break;
    }
    case StubIntrinsic::IsArray: {
        checkCallee(Entry::HostArrayIsArray);
        Jump notCell = jit.branchIfNotCell(first);
        jit.load8(Address(first, JSCell::typeInfoTypeOffset()), scratch0);
        needsHostFunction.append(jit.branch32(CCallHelpers::Equal, scratch0, TrustedImm32(ProxyObjectType)));
        static_assert(ArrayType + 1 == DerivedArrayType);
        jit.sub32(TrustedImm32(ArrayType), scratch0);
        Jump notArray = jit.branch32(CCallHelpers::Above, scratch0, TrustedImm32(DerivedArrayType - ArrayType));
        returnConstant(jsBoolean(true));
        notCell.link(&jit);
        notArray.link(&jit);
        returnConstant(jsBoolean(false));
        break;
    }
    case StubIntrinsic::Get:
    case StubIntrinsic::Has:
    case StubIntrinsic::Set:
    case StubIntrinsic::SetIgnoringResult:
    case StubIntrinsic::Add:
    case StubIntrinsic::AddIgnoringResult: {
        hasAnotherReceiver.append(jit.branchIfNotCell(receiver));
        switch (intrinsic) {
        case StubIntrinsic::Get:
            hasAnotherReceiver.append(jit.branchIfNotType(receiver, JSMapType));
            checkCallee(Entry::HostMapGet);
            generateMapOrSetOperation(jit, MapOrSetOperation::MapGet, &needsHostFunction);
            break;
        case StubIntrinsic::Has: {
            Jump isSet = jit.branchIfType(receiver, JSSetType);
            hasAnotherReceiver.append(jit.branchIfNotType(receiver, JSMapType));
            checkCallee(Entry::HostMapHas);
            generateMapOrSetOperation(jit, MapOrSetOperation::MapHas, &needsHostFunction);
            isSet.link(&jit);
            checkCallee(Entry::HostSetHas);
            generateMapOrSetOperation(jit, MapOrSetOperation::SetHas, &needsHostFunction);
            break;
        }
        case StubIntrinsic::Set:
        case StubIntrinsic::SetIgnoringResult:
            hasAnotherReceiver.append(jit.branchIfNotType(receiver, JSMapType));
            checkCallee(Entry::HostMapSet);
            generateMapOrSetOperation(jit, MapOrSetOperation::MapSet, &needsHostFunction);
            break;
        default:
            hasAnotherReceiver.append(jit.branchIfNotType(receiver, JSSetType));
            checkCallee(Entry::HostSetAdd);
            generateMapOrSetOperation(jit, MapOrSetOperation::SetAdd, &needsHostFunction);
            break;
        }
        break;
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

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

static void generateCreateRestWithFastPath(CCallHelpers& jit)
{
    generateAheadOf(jit, Entry::CreateRestSlowPath, [&] {
        jit.zeroExtend32ToWord(A3, A3);
        jit.getEffectiveAddress(CCallHelpers::BaseIndex(A2, A3, CCallHelpers::TimesEight), A0);
        Jump some = jit.branch32(CCallHelpers::Above, A1, A3);
        jit.move(A3, A1);
        some.link(&jit);
        jit.sub32(A3, A1);
        callStubFromStub(jit, Stub::HelperNewArray);
    });
}

static void generateNewInternalFieldObjectWithFastPath(CCallHelpers& jit)
{
    generateAheadOf(jit, Entry::NewInternalFieldObjectSlowPath, [&] {
        Jump isGenerator = jit.branch32(CCallHelpers::Equal, A1, TrustedImm32(static_cast<uint32_t>(InternalFieldObjectKind::Generator)));
        Jump isAsyncFunctionGenerator = jit.branch32(CCallHelpers::Equal, A1, TrustedImm32(static_cast<uint32_t>(InternalFieldObjectKind::AsyncFunctionGenerator)));
        callStubFromStub(jit, Stub::HelperNewPromise);
        Jump isPromise = jit.jump();
        isGenerator.link(&jit);
        callStubFromStub(jit, Stub::HelperNewGenerator);
        Jump wasGenerator = jit.jump();
        isAsyncFunctionGenerator.link(&jit);
        callStubFromStub(jit, Stub::HelperNewAsyncFunctionGenerator);
        isPromise.link(&jit);
        wasGenerator.link(&jit);
    });
}

static void generateNewMapOrSetWithFastPath(CCallHelpers& jit)
{
    generateAheadOf(jit, Entry::NewMapOrSetSlowPath, [&] {
        Jump isSet = jit.branchTest32(CCallHelpers::NonZero, A1);
        callStubFromStub(jit, Stub::HelperNewMap);
        Jump isMap = jit.jump();
        isSet.link(&jit);
        callStubFromStub(jit, Stub::HelperNewSet);
        isMap.link(&jit);
    });
}

static void generateToStringWithFastPath(CCallHelpers& jit)
{
    generateAheadOf(jit, Entry::ToStringSlowPath, [&] {
        Jump isInt32 = jit.branchIfInt32(A1);
        jit.move(TrustedImm32(0), GPRInfo::returnValueGPR);
        Jump isNotInt32 = jit.jump();
        isInt32.link(&jit);
        jit.move(A1, A0);
        callStubFromStub(jit, Stub::HelperInt32ToString);
        isNotInt32.link(&jit);
    });
}

static void generateInt32ToStringWithValidRadixWithFastPath(CCallHelpers& jit)
{
    generateAheadOf(jit, Entry::Int32ToStringWithValidRadixSlowPath, [&] {
        Jump isDecimal = jit.branch32(CCallHelpers::Equal, A2, TrustedImm32(10));
        jit.move(TrustedImm32(0), GPRInfo::returnValueGPR);
        Jump isNotDecimal = jit.jump();
        isDecimal.link(&jit);
        jit.move(A1, A0);
        callStubFromStub(jit, Stub::HelperInt32ToString);
        isNotDecimal.link(&jit);
    });
}

static void generateNewResolvedPromiseWithFastPath(CCallHelpers& jit) { generateAheadOf(jit, Entry::NewResolvedPromiseSlowPath, Stub::HelperNewResolvedPromise, 1); }
static void generateNewArrayBufferWithFastPath(CCallHelpers& jit) { generateAheadOf(jit, Entry::NewArrayBufferSlowPath, Stub::HelperNewArrayBuffer, 1); }
static void generateNewArrayWithSpreadWithFastPath(CCallHelpers& jit) { generateAheadOf(jit, Entry::NewArrayWithSpreadSlowPath, Stub::HelperNewArrayWithSpread, 3); }
static void generateNewArrayWithSpeciesWithFastPath(CCallHelpers& jit) { generateAheadOf(jit, Entry::NewArrayWithSpeciesSlowPath, Stub::HelperNewArrayWithSpecies, 2); }
static void generateNewArrayWithSizeWithFastPath(CCallHelpers& jit) { generateAheadOf(jit, Entry::NewArrayWithSizeSlowPath, Stub::HelperNewArrayWithSize, 1); }
static void generateCreateLexicalEnvironmentWithFastPath(CCallHelpers& jit) { generateAheadOf(jit, Entry::CreateLexicalEnvironmentSlowPath, Stub::HelperNewActivation, 4); }
static void generateMakeRope2WithFastPath(CCallHelpers& jit) { generateAheadOf(jit, Entry::MakeRope2SlowPath, Stub::HelperMakeRope2, 2); }
static void generateMakeRope3WithFastPath(CCallHelpers& jit) { generateAheadOf(jit, Entry::MakeRope3SlowPath, Stub::HelperMakeRope3, 3); }
static void generateStringSliceWithEndWithFastPath(CCallHelpers& jit) { generateAheadOf(jit, Entry::StringSliceWithEndSlowPath, Stub::HelperStringSlice, 3); }
static void generateStringSubstringWithEndWithFastPath(CCallHelpers& jit) { generateAheadOf(jit, Entry::StringSubstringWithEndSlowPath, Stub::HelperStringSubstring, 3); }
static void generateValueAddWithFastPath(CCallHelpers& jit) { generateAheadOf(jit, Entry::ValueAddSlowPath, Stub::HelperAddStrings, 2); }
static void generateStrcatWithFastPath(CCallHelpers& jit) { generateAheadOf(jit, Entry::StrcatSlowPath, Stub::HelperStrcat, 2); }
static void generateObjectKeysObjectWithFastPath(CCallHelpers& jit) { generateAheadOf(jit, Entry::ObjectKeysObjectSlowPath, Stub::HelperObjectKeys, 1); }

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
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfVM()), scratch);
    Jump isNotNeeded = jit.branchTest8(CCallHelpers::Zero, Address(scratch, VM::offsetOfHeapMutatorShouldBeFenced()));
    jit.storeFence();
    isNotNeeded.link(&jit);
}

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

static void generateNewObjectLiteral(CCallHelpers& jit, unsigned count)
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
#else
    constexpr GPRReg scratch = A0;
#endif
    static_assert(noOverlap(cache, object, allocator, scratch, A2, A3, A4, A5));
    static_assert(JSObject::butterflyOffset() == sizeof(EncodedJSValue));
    unsigned end = WTF::roundUpToMultipleOf<2>(count);

    CCallHelpers::JumpList slowCases;
    slowCases.append(jit.branchTest32(CCallHelpers::Zero, Address(cache, OBJECT_OFFSETOF(Slot, structureID))));
    jit.loadPtr(Address(cache, sizeof(Slot) + OBJECT_OFFSETOF(Slot, pointer)), allocator);
    jit.emitAllocateWithNonNullAllocator(object, JITAllocator::variable(), allocator, scratch, slowCases, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
    jit.load32(Address(cache, OBJECT_OFFSETOF(Slot, structureID)), scratch);
    jit.load64(Address(cache, sizeof(Slot)), allocator);
    jit.or64(allocator, scratch);
#if CPU(ARM64)
    jit.storePair64(scratch, ARM64Registers::zr, object, TrustedImm32(0));
#else
    jit.store64(scratch, Address(object, 0));
    jit.store64(TrustedImm32(0), Address(object, JSObject::butterflyOffset()));
#endif
    storeValuesAndClear(jit, 2, count, end, object, JSObject::offsetOfInlineStorage());
    jit.load32(Address(cache, OBJECT_OFFSETOF(Slot, offset)), allocator);
#if CPU(ARM64)
    // Two at a time, from the last pair down. A cell is a multiple of 16 bytes.
    static_assert(JSObject::offsetOfInlineStorage() == 2 * sizeof(EncodedJSValue));
    jit.add32(TrustedImm32(1), allocator);
    jit.and32(TrustedImm32(Slot::offsetMask & ~1u), allocator);
    Jump isCleared = jit.branch32(CCallHelpers::BelowOrEqual, allocator, TrustedImm32(end));
    jit.getEffectiveAddress(CCallHelpers::BaseIndex(object, allocator, CCallHelpers::TimesEight), allocator);
    GPRReg lastPairOfValues = object;
    if (end) {
        jit.addPtr(TrustedImm32(end * sizeof(EncodedJSValue)), object, scratch);
        lastPairOfValues = scratch;
    }
    auto clear = jit.label();
    jit.storePair64(ARM64Registers::zr, ARM64Registers::zr, CCallHelpers::PostIndexAddress(allocator, -2 * static_cast<int>(sizeof(EncodedJSValue))));
    jit.branchPtr(CCallHelpers::Above, allocator, lastPairOfValues).linkTo(clear, &jit);
    isCleared.link(&jit);
#else
    jit.and32(TrustedImm32(Slot::offsetMask), allocator);
    auto clear = jit.label();
    Jump isCleared = jit.branch32(CCallHelpers::BelowOrEqual, allocator, TrustedImm32(end));
    jit.sub32(TrustedImm32(1), allocator);
    jit.store64(TrustedImm32(0), CCallHelpers::BaseIndex(object, allocator, CCallHelpers::TimesEight, JSObject::offsetOfInlineStorage()));
    jit.jump().linkTo(clear, &jit);
    isCleared.link(&jit);
#endif
    fenceAfterAllocation(jit, scratch);
    jit.ret();

    slowCases.link(&jit);
    callWithValuesOnStack(jit, Entry::operationAOTNewObjectLiteral, 2, count, [&] {
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
    slowCases.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    jit.emitAllocateWithNonNullAllocator(array, JITAllocator::variable(), allocator, scratch, slowCases, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
    jit.store64(CCallHelpers::TrustedImm64(static_cast<uint64_t>(vectorLength) << 32 | count), Address(storage, 0));
    storeValuesAndClear(jit, 1, count, vectorLength, storage, sizeof(IndexingHeader));
    jit.addPtr(TrustedImm32(sizeof(IndexingHeader)), storage);
    jit.load32(structureID, scratch);
    jit.or64(CCallHelpers::TrustedImm64(static_cast<uint64_t>(TypeInfoBlob(ArrayWithContiguous, TypeInfo(ArrayType, JSArray::StructureFlags)).blob()) << 32), scratch);
    jit.storePair64(scratch, storage, array, TrustedImm32(0));
    fenceAfterAllocation(jit, scratch);
    returnToTypedCaller(jit, isLeaf);

    slowCases.link(&jit);
#if CPU(ARM64)
    jit.move(TrustedImm32(static_cast<unsigned>(Entry::operationAOTTypedNewArray) * sizeof(void*)), T9);
    generateColdOperation(jit, isLeaf, true, true, true, false, true, countAndCapacity);
#else
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(WTF::roundUpToMultipleOf<stackAlignmentBytes()>(std::max(count, 1u) * sizeof(EncodedJSValue))), sp);
    for (unsigned i = 0; i < count; ++i)
        jit.store64(GPRInfo::toArgumentRegister(1 + i), Address(sp, i * sizeof(EncodedJSValue)));
    jit.move(sp, A2);
    jit.move(TrustedImm32(countAndCapacity), A1);
    loadInstance(jit, A0);
    jit.loadPtr(Address(A0, Instance::offsetOfRuntimeTable()), operationGPR);
    jit.loadPtr(Address(operationGPR, static_cast<unsigned>(Entry::operationAOTTypedNewArray) * sizeof(void*)), operationGPR);
    prepareCallOperation(jit, assertionScratchGPR);
    jit.call(operationGPR, OperationPtrTag);
    jit.emitFunctionEpilogue();
    Jump exception = jit.branchTestPtr(CCallHelpers::NonZero, GPRInfo::returnValueGPR2);
    jit.ret();
    exception.link(&jit);
    loadInstance(jit, T9);
    jumpToEntry(jit, T9, Entry::HandleException);
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
static void generateArrayOfNumbers(CCallHelpers& jit, IndexingType indexingType, bool isLeaf, unsigned capacityOfEmpty = 0)
{
    constexpr GPRReg array = GPRInfo::returnValueGPR;
    constexpr GPRReg allocator = T9;
#if CPU(ARM64)
    constexpr GPRReg scratch = T11;
    constexpr GPRReg storage = T12;
    constexpr GPRReg capacity = T13;
    constexpr GPRReg length = T14;
#else
    constexpr GPRReg scratch = A0;
    constexpr GPRReg storage = A5;
    constexpr GPRReg capacity = A1;
    constexpr GPRReg length = A2;
#endif
    static_assert(noOverlap(array, allocator, scratch, storage, capacity, length));
    static_assert(sizeof(IndexingHeader) == sizeof(EncodedJSValue) && MarkedSpace::sizeStep == 2 * sizeof(EncodedJSValue) && sizeof(Allocator) == sizeof(void*));
    static_assert(sizeof(IndexingHeader) + maxCapacityOfArrayOfNumbers * sizeof(EncodedJSValue) <= MarkedSpace::largeCutoff && maxCapacityOfArrayOfNumbers & 1);
    RELEASE_ASSERT(indexingType == ArrayWithDouble || indexingType == ArrayWithInt32);
    unsigned kind = (indexingType & (IndexingShapeMask | CopyOnWrite)) >> Instance::arrayKindShift;

    CCallHelpers::JumpList failed;
    // Zero once arrays are no longer allocated with their original structures.
    Jump isNotToBeHad = jit.branchTest32(CCallHelpers::Zero, Address(instanceGPR, Instance::offsetOfNewArrayWithContiguousStructureID()));
    bool isEmpty = capacityOfEmpty;
    RELEASE_ASSERT(!isEmpty || capacityOfEmpty & 1);
    if (isEmpty) {
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfAuxiliarySpace()), allocator);
        jit.loadPtr(Address(allocator, CompleteSubspace::offsetOfAllocatorForSizeStep() + MarkedSpace::sizeClassToIndex(sizeof(IndexingHeader) + capacityOfEmpty * sizeof(EncodedJSValue)) * sizeof(Allocator)), allocator);
    } else {
        jit.and32(TrustedImm32((1 << capacityShiftInArrayOfNumbers) - 1), T9, length);
        jit.urshift32(T9, TrustedImm32(capacityShiftInArrayOfNumbers), capacity);
        failed.append(jit.branch32(CCallHelpers::Above, capacity, TrustedImm32(maxCapacityOfArrayOfNumbers)));
        // With the header an even number of words.
        jit.add32(TrustedImm32(1), capacity, scratch);
        jit.urshift32(TrustedImm32(1), scratch);
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfAuxiliarySpace()), allocator);
        jit.loadPtr(CCallHelpers::BaseIndex(allocator, scratch, CCallHelpers::TimesEight, CompleteSubspace::offsetOfAllocatorForSizeStep()), allocator);
    }
    failed.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    jit.emitAllocateWithNonNullAllocator(storage, JITAllocator::variable(), allocator, scratch, failed, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfArrayAllocator()), allocator);
    failed.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
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
        Jump isFull = jit.branch32(CCallHelpers::AboveOrEqual, length, capacity);
        jit.move(hole, scratch);
        CCallHelpers::Label fill = jit.label();
        jit.store64(scratch, CCallHelpers::BaseIndex(storage, length, CCallHelpers::TimesEight));
        jit.add32(TrustedImm32(1), length);
        jit.branch32(CCallHelpers::Below, length, capacity).linkTo(fill, &jit);
        isFull.link(&jit);
    }
    jit.load32(Address(instanceGPR, Instance::offsetOfOriginalArrayStructureIDs() + kind * sizeof(uint32_t)), scratch);
    jit.or64(CCallHelpers::TrustedImm64(static_cast<uint64_t>(TypeInfoBlob(indexingType, TypeInfo(ArrayType, JSArray::StructureFlags)).blob()) << 32), scratch);
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
    if (isEmpty)
        jit.move(TrustedImm32(capacityOfEmpty << capacityShiftInArrayOfNumbers), A1);
    else {
        static_assert(capacity == A1 && length == A2);
        jit.lshift32(TrustedImm32(capacityShiftInArrayOfNumbers), A1);
        jit.or32(A2, A1);
    }
    jit.move(TrustedImm32(indexingType), A2);
    jit.emitFunctionPrologue();
    loadInstance(jit, A0);
    jit.loadPtr(Address(A0, Instance::offsetOfRuntimeTable()), operationGPR);
    jit.loadPtr(Address(operationGPR, static_cast<unsigned>(Entry::operationAOTTypedNewArrayOfNumbers) * sizeof(void*)), operationGPR);
    prepareCallOperation(jit, assertionScratchGPR);
    jit.call(operationGPR, OperationPtrTag);
    jit.emitFunctionEpilogue();
    Jump exception = jit.branchTestPtr(CCallHelpers::NonZero, GPRInfo::returnValueGPR2);
    jit.ret();
    exception.link(&jit);
    loadInstance(jit, T9);
    jumpToEntry(jit, T9, Entry::HandleException);
#endif
}
static void generateArrayOfDoubles(CCallHelpers& jit) { generateArrayOfNumbers(jit, ArrayWithDouble, false); }
static void generateArrayOfInt32s(CCallHelpers& jit) { generateArrayOfNumbers(jit, ArrayWithInt32, false); }
static void generateEmptyArrayOfDoubles(CCallHelpers& jit) { generateArrayOfNumbers(jit, ArrayWithDouble, false, vectorLengthOfArrayLiteral(0)); }
static void generateEmptyArrayOfInt32s(CCallHelpers& jit) { generateArrayOfNumbers(jit, ArrayWithInt32, false, vectorLengthOfArrayLiteral(0)); }
static void generateEmptyArrayOfDoublesWithRoomFor1(CCallHelpers& jit) { generateArrayOfNumbers(jit, ArrayWithDouble, false, 1); }
static void generateEmptyArrayOfDoublesWithRoomFor3(CCallHelpers& jit) { generateArrayOfNumbers(jit, ArrayWithDouble, false, 3); }
#if CPU(ARM64)
static void generateLeafArrayOfDoubles(CCallHelpers& jit) { generateArrayOfNumbers(jit, ArrayWithDouble, true); }
static void generateLeafArrayOfInt32s(CCallHelpers& jit) { generateArrayOfNumbers(jit, ArrayWithInt32, true); }
static void generateLeafEmptyArrayOfDoubles(CCallHelpers& jit) { generateArrayOfNumbers(jit, ArrayWithDouble, true, vectorLengthOfArrayLiteral(0)); }
static void generateLeafEmptyArrayOfInt32s(CCallHelpers& jit) { generateArrayOfNumbers(jit, ArrayWithInt32, true, vectorLengthOfArrayLiteral(0)); }
static void generateLeafEmptyArrayOfDoublesWithRoomFor1(CCallHelpers& jit) { generateArrayOfNumbers(jit, ArrayWithDouble, true, 1); }
static void generateLeafEmptyArrayOfDoublesWithRoomFor3(CCallHelpers& jit) { generateArrayOfNumbers(jit, ArrayWithDouble, true, 3); }
#else
static void generateLeafArrayOfDoubles(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafArrayOfInt32s(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafEmptyArrayOfDoubles(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafEmptyArrayOfInt32s(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafEmptyArrayOfDoublesWithRoomFor1(CCallHelpers& jit) { jit.breakpoint(); }
static void generateLeafEmptyArrayOfDoublesWithRoomFor3(CCallHelpers& jit) { jit.breakpoint(); }
#endif

// For code compiled from types: `array.push(value)` that has found no room: operationAOTTypedArrayPush's arguments from the second argument register on: the array, the value, how many more are
// expected. Yields what that yields. A vector of 1 or of 3 values that is full grows here, to what growVector() would give it, so that both take the same size class. All else is the operation's:
// another structure than arrays of values are made with, an array that has to be remembered, a collection under way.
static void generatePushToFullVector(CCallHelpers& jit, bool isLeaf)
{
    constexpr GPRReg result = GPRInfo::returnValueGPR;
    constexpr GPRReg allocator = T9;
#if CPU(ARM64)
    constexpr GPRReg scratch = T11;
    constexpr GPRReg storage = T12;
    constexpr GPRReg old = T13;
#else
    constexpr GPRReg scratch = A0;
    constexpr GPRReg storage = A5;
    constexpr GPRReg old = A4;
#endif
    static_assert(noOverlap(result, allocator, scratch, storage, old, A1, A2, A3));
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
        jit.move(A1, T9);
        jit.move(A2, T11);
        jit.jump().linkTo(stubLabels()[static_cast<unsigned>(isLeaf ? Stub::LeafRememberElement : Stub::RememberElement)], &jit);
        isNaN.link(&jit);
        jit.move64ToDouble(storage, number);
        hasNoRoom.link(&jit);
        doesNotKeepValues.link(&jit);
        keepsSomethingElse.link(&jit);
        isNoInt32.link(&jit);
    }
    // Where it is known how many are to come the operation makes room for all of them at once.
    slowCases.append(jit.branchTest32(CCallHelpers::NonZero, A3));
    jit.load32(Address(A1, JSCell::structureIDOffset()), scratch);
    slowCases.append(jit.branch32(CCallHelpers::NotEqual, scratch, Address(instanceGPR, Instance::offsetOfNewArrayWithContiguousStructureID())));
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfVM()), scratch);
    slowCases.append(jit.branchTest8(CCallHelpers::NonZero, Address(scratch, VM::offsetOfHeapMutatorShouldBeFenced())));
    jit.load8(Address(A1, JSCell::cellStateOffset()), result);
    slowCases.append(jit.branch32(CCallHelpers::BelowOrEqual, result, Address(scratch, VM::offsetOfHeapBarrierThreshold())));
    jit.loadPtr(Address(A1, JSObject::butterflyOffset()), old);
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
    slowCases.append(jit.branch64(CCallHelpers::NotEqual, result, lengths(3, 3)));
    grow(3, 7);
    hasOne.link(&jit);
    grow(1, 3);

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

#define AOT_GENERATE_NEW_OBJECT_LITERAL(n) static void generateNewObjectLiteral##n(CCallHelpers& jit) { generateNewObjectLiteral(jit, n); }
AOT_GENERATE_NEW_OBJECT_LITERAL(0)
AOT_GENERATE_NEW_OBJECT_LITERAL(1)
AOT_GENERATE_NEW_OBJECT_LITERAL(2)
AOT_GENERATE_NEW_OBJECT_LITERAL(3)
AOT_GENERATE_NEW_OBJECT_LITERAL(4)
AOT_GENERATE_NEW_OBJECT_LITERAL(5)
AOT_GENERATE_NEW_OBJECT_LITERAL(6)
#undef AOT_GENERATE_NEW_OBJECT_LITERAL

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

#define AOT_GENERATE_NEW_ARRAY_LITERAL(n) \
static void generateNewArrayLiteral##n(CCallHelpers& jit) { generateNewArrayLiteral(jit, n, false); } \
static void generateNewInt32ArrayLiteral##n(CCallHelpers& jit) { generateNewArrayLiteral(jit, n, true); }
AOT_GENERATE_NEW_ARRAY_LITERAL(1)
AOT_GENERATE_NEW_ARRAY_LITERAL(2)
AOT_GENERATE_NEW_ARRAY_LITERAL(3)
AOT_GENERATE_NEW_ARRAY_LITERAL(4)
#undef AOT_GENERATE_NEW_ARRAY_LITERAL

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
    Stub::OperationValue, Stub::OperationVoid, Stub::OperationDouble, Stub::OperationValueWithGlobalObject, Stub::OperationVoidWithGlobalObject, Stub::OperationDoubleWithGlobalObject,
    Stub::OperationValueWithInstance, Stub::OperationVoidWithInstance, Stub::OperationDoubleWithInstance,
    Stub::PlainOperation, Stub::PlainOperationWithGlobalObject, Stub::PlainOperationWithInstance, Stub::PlainOperationWithVM,
    Stub::ColdOperationVoid, Stub::LeafColdOperationVoid, Stub::ColdOperationValue, Stub::LeafColdOperationValue,
    Stub::ColdPlainOperation, Stub::LeafColdPlainOperation, Stub::ColdPlainOperationWithoutInstance, Stub::LeafColdPlainOperationWithoutInstance,
    Stub::ColdPlainOperationDouble, Stub::LeafColdPlainOperationDouble, Stub::ColdPlainOperationDoubleWithoutInstance, Stub::LeafColdPlainOperationDoubleWithoutInstance,
    Stub::ColdOperationDouble, Stub::LeafColdOperationDouble,
    Stub::ColdOperationVoidWithoutFPRs, Stub::LeafColdOperationVoidWithoutFPRs, Stub::ColdOperationValueWithoutFPRs, Stub::LeafColdOperationValueWithoutFPRs,
    Stub::ColdPlainOperationWithoutFPRs, Stub::LeafColdPlainOperationWithoutFPRs,
};
static constexpr Stub functionCallStubs[] = { Stub::Call, Stub::Construct, Stub::CallHostFunction, Stub::CallInternalFunction, Stub::ConstructInternalFunction, Stub::CallCached, Stub::CallValue, Stub::ConstructClosure, Stub::CallMethod, Stub::CallMethodRead };
static constexpr unsigned numberOfCountsWithThunk = numberOfArgumentGPRs + 1;
static constexpr unsigned firstCallThunk = std::size(operationCallStubs) * numberOfEntries;
static constexpr unsigned firstCallThunkOf(Stub stub)
{
    unsigned index = 0;
    while (functionCallStubs[index] != stub)
        ++index;
    return firstCallThunk + index * numberOfCountsWithThunk;
}
static constexpr unsigned firstIntrinsicThunk = firstCallThunk + std::size(functionCallStubs) * numberOfCountsWithThunk;
static constexpr Stub stubsWithAnyRegisterOperand[] = { Stub::WriteBarrier, Stub::ToBoolean, Stub::GetById, Stub::GetByIdWellKnown,
    Stub::PutById, Stub::GetByVal, Stub::GetByValAtIndex, Stub::PutByVal, Stub::PutByValAtIndex, Stub::PutByValDirect, Stub::GetFromScope, Stub::GetGlobal, Stub::ResolveScope,
    Stub::StrictEqual, Stub::LooseEqual, Stub::IsStringEqualTo, Stub::GetLength, Stub::HelperAddField,
    Stub::ReadSlot0, Stub::ReadSlot1, Stub::ReadSlot2, Stub::ReadSlot3, Stub::ReadSlot4, Stub::ReadSlot5, Stub::ReadSlot6, Stub::ReadSlot7,
    Stub::ReadSlot8, Stub::ReadSlot9, Stub::ReadSlot10, Stub::ReadSlot11, Stub::ReadSlot12, Stub::ReadSlot13, Stub::ReadSlot14, Stub::ReadSlot15,
    Stub::ReadSlotOrUndefined0, Stub::ReadSlotOrUndefined1, Stub::ReadSlotOrUndefined2, Stub::ReadSlotOrUndefined3, Stub::ReadSlotOrUndefined4, Stub::ReadSlotOrUndefined5, Stub::ReadSlotOrUndefined6, Stub::ReadSlotOrUndefined7,
    Stub::ReadSlotOrUndefined8, Stub::ReadSlotOrUndefined9, Stub::ReadSlotOrUndefined10, Stub::ReadSlotOrUndefined11, Stub::ReadSlotOrUndefined12, Stub::ReadSlotOrUndefined13, Stub::ReadSlotOrUndefined14, Stub::ReadSlotOrUndefined15 };
struct OperationWithAnyRegisterOperand {
    Stub stub;
    Entry operation;
};
static constexpr OperationWithAnyRegisterOperand operationsWithAnyRegisterOperand[] = {
    { Stub::ColdOperationVoid, Entry::operationAOTCheckType }, { Stub::LeafColdOperationVoid, Entry::operationAOTCheckType },
    { Stub::ColdOperationVoid, Entry::operationAOTCheckTypedLayout }, { Stub::LeafColdOperationVoid, Entry::operationAOTCheckTypedLayout },
    { Stub::ColdOperationValue, Entry::operationAOTGetElementOrEmpty }, { Stub::LeafColdOperationValue, Entry::operationAOTGetElementOrEmpty },
    { Stub::ColdOperationValue, Entry::operationAOTGetByVal }, { Stub::LeafColdOperationValue, Entry::operationAOTGetByVal },
    { Stub::OperationValueWithInstance, Entry::operationAOTNewFunction },
    { Stub::OperationValueWithInstance, Entry::operationAOTToString },
    { Stub::OperationValueWithInstance, Entry::operationAOTToThis },
    { Stub::OperationValueWithInstance, Entry::operationAOTCreateLexicalEnvironment },
    { Stub::OperationValueWithInstance, Entry::operationAOTCloneObject },
    { Stub::OperationValueWithInstance, Entry::operationAOTReadLazyClosureVar },
    { Stub::OperationVoidWithInstance, Entry::operationAOTThrow },
    { Stub::OperationVoidWithInstance, Entry::operationAOTThrowNotAFunction },
    { Stub::PlainOperationWithInstance, Entry::operationAOTToBoolean },
};
static constexpr unsigned numberOfRegistersForOperand = 30;
static constexpr unsigned firstOperandThunk = firstIntrinsicThunk + numberOfStubIntrinsics;
static constexpr unsigned firstOperationOperandThunk = firstOperandThunk + std::size(stubsWithAnyRegisterOperand) * numberOfRegistersForOperand;
static constexpr Stub stubsWithTwoAnyRegisterOperands[] = { Stub::StrictEqual, Stub::LooseEqual, Stub::PutById, Stub::GetByVal, Stub::GetByValAtIndex, Stub::PutByVal, Stub::PutByValAtIndex, Stub::PutByValDirect, Stub::HelperAddField };
static constexpr unsigned numberOfRegistersForPair = 16;
struct OperationWithAnyRegisterResult {
    Entry operation;
    bool acceptsOperandInAnyRegister;
};
static constexpr OperationWithAnyRegisterResult operationsWithAnyRegisterResult[] = {
    { Entry::operationAOTNewFunction, true }, { Entry::operationAOTToString, true }, { Entry::operationAOTToThis, true }, { Entry::operationAOTCreateLexicalEnvironment, true },
    { Entry::operationAOTCloneObject, true }, { Entry::operationAOTReadLazyClosureVar, true },
    { Entry::operationAOTNewObjectLiteral, false }, { Entry::operationAOTNewInternalFieldObject, false }, { Entry::operationAOTNewObject, false }, { Entry::operationAOTNewArray, false },
    { Entry::operationAOTNewArrayWithSpecies, false }, { Entry::operationMakeRope2, false }, { Entry::operationMakeRope3, false },
};
static constexpr unsigned numberOfRegistersForResult = 7;
static constexpr unsigned firstPairThunk = firstOperationOperandThunk + std::size(operationsWithAnyRegisterOperand) * numberOfRegistersForOperand;
static constexpr unsigned firstResultThunk = firstPairThunk + std::size(stubsWithTwoAnyRegisterOperands) * numberOfRegistersForPair * numberOfRegistersForPair;
static constexpr unsigned numberOfThunks = firstResultThunk + std::size(operationsWithAnyRegisterResult) * numberOfRegistersForResult * numberOfRegistersForOperand;
static_assert(numberOfThunks < std::numeric_limits<uint16_t>::max());

static bool hasNoPerRegisterEntrypoints()
{
    return !isARM64();
}

static std::optional<unsigned> firstOperandThunkFor(Stub stub, std::optional<uint32_t> t9Value)
{
    if (hasNoPerRegisterEntrypoints())
        return std::nullopt;
    if (!t9Value) {
        for (unsigned i = 0; i < std::size(stubsWithAnyRegisterOperand); ++i) {
            if (stubsWithAnyRegisterOperand[i] == stub)
                return firstOperandThunk + i * numberOfRegistersForOperand;
        }
        return std::nullopt;
    }
    for (unsigned i = 0; i < std::size(operationsWithAnyRegisterOperand); ++i) {
        if (operationsWithAnyRegisterOperand[i].stub == stub && static_cast<unsigned>(operationsWithAnyRegisterOperand[i].operation) * sizeof(void*) == *t9Value)
            return firstOperationOperandThunk + i * numberOfRegistersForOperand;
    }
    return std::nullopt;
}

static bool callsOperation(Stub stub)
{
    for (Stub other : operationCallStubs) {
        if (other == stub)
            return true;
    }
    return false;
}

GPRReg defaultOperandRegister(Stub stub)
{
    return callsOperation(stub) ? GPRInfo::argumentGPR1 : isHelper(stub) ? GPRInfo::argumentGPR0 : firstStubOperandGPR;
}

bool preservesOperandRegister(Stub stub)
{
    return stub == Stub::WriteBarrier;
}

std::optional<RegisterSet> registersChangedBy(Stub stub)
{
    unsigned numberOfTemporaries = 0;
    switch (stub) {
    case Stub::LinkFunction:
        return RegisterSet { };
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
        numberOfTemporaries = 2;
        break;
    case Stub::WriteBarrier:
    case Stub::ToBoolean:
    case Stub::Latin1Characters:
    case Stub::WeakMapGet:
    case Stub::WeakMapHas:
    case Stub::WeakSetHas:
        numberOfTemporaries = 3;
        break;
    case Stub::Constant:
    case Stub::TemplateObject:
    case Stub::TransientConstant:
    case Stub::ConstantFromSlot:
        numberOfTemporaries = isARM64() ? 7 : 3;
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
    case Stub::TypedString:
    case Stub::LeafTypedString:
    case Stub::StringIsText:
    case Stub::LeafStringIsText:
    case Stub::StringStartsWithText:
    case Stub::LeafStringStartsWithText:
    case Stub::StringEndsWithText:
    case Stub::LeafStringEndsWithText:
    case Stub::StringsAreEqual:
    case Stub::LeafStringsAreEqual:
    case Stub::TypedClosure:
    case Stub::LeafTypedClosure:
    case Stub::TypedBuiltinCell:
    case Stub::LeafTypedBuiltinCell:
    case Stub::TypedClosureEntry:
    case Stub::LeafTypedClosureEntry:
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
    case Stub::ArrayOfDoubles:
    case Stub::LeafArrayOfDoubles:
    case Stub::ArrayOfInt32s:
    case Stub::LeafArrayOfInt32s:
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
    case Stub::PushToFullVector:
    case Stub::LeafPushToFullVector:
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
    return result;
}

void dumpRegistersChangedByStubs(PrintStream& out)
{
    for (unsigned i = 0; i < numberOfStubs; ++i) {
        Stub stub = static_cast<Stub>(i);
        auto changed = registersChangedBy(stub);
        if (!changed)
            continue;
        bool keepsReturnAddressOfCaller = stub == Stub::Constant || stub == Stub::TemplateObject || stub == Stub::TransientConstant || stub == Stub::ConstantFromSlot;
        if (keepsReturnAddressOfCaller)
            changed->remove(stubTemporaryGPRs[1]);
        bool hasResult = stub != Stub::LinkFunction && stub != Stub::WriteBarrier && stub != Stub::ColdOperationVoid && stub != Stub::LeafColdOperationVoid && stub != Stub::ColdOperationVoidWithoutFPRs && stub != Stub::LeafColdOperationVoidWithoutFPRs
            && stub != Stub::ColdCallFunction && stub != Stub::LeafColdCallFunction;
        if (hasResult)
            changed->add(GPRInfo::returnValueGPR, IgnoreVectors);
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

bool operandAllowedInRegister(Stub stub, GPRReg reg)
{
#if CPU(ARM64)
    unsigned number = static_cast<unsigned>(reg) - static_cast<unsigned>(ARM64Registers::x0);
    if (number >= numberOfRegistersForOperand || number == 16 || number == 17)
        return false;
    if (number == 18)
        return false;
    if (callsOperation(stub))
        return true;
    switch (stub) {
    case Stub::WriteBarrier:
    case Stub::ToBoolean:
        return number < 9 || number > 11;
    case Stub::GetById:
    case Stub::GetByIdWellKnown:
        return (number < 9 && reg != GPRInfo::argumentGPR1) || number > 15;
    default:
        if (slotReadBy(stub))
            return (number < 9 && reg != GPRInfo::argumentGPR1) || number > 15;
        return true;
    }
#else
    UNUSED_PARAM(stub);
    UNUSED_PARAM(reg);
    return false;
#endif
}

unsigned thunkForOperandRegister(Stub stub, std::optional<uint32_t> t9Value, GPRReg reg)
{
    RELEASE_ASSERT(operandAllowedInRegister(stub, reg));
    return *firstOperandThunkFor(stub, t9Value) + static_cast<unsigned>(reg);
}

static GPRReg registerForPair(unsigned number)
{
    return static_cast<GPRReg>(number <= 8 ? number : number + 10);
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
    if (stub == Stub::CallIntrinsic) {
        RELEASE_ASSERT(t9Value && t9Value <= numberOfStubIntrinsics);
        return firstIntrinsicThunk + t9Value - 1;
    }
    return std::nullopt;
}

static void moveToFirstTwoArguments(CCallHelpers& jit, GPRReg first, GPRReg second)
{
    constexpr GPRReg a0 = GPRInfo::argumentGPR0;
    constexpr GPRReg a1 = GPRInfo::argumentGPR1;
    if (second == a0 && first == a1) {
        jit.swap(a0, a1);
        return;
    }
    if (second == a0) {
        jit.move(second, a1);
        jit.move(first, a0);
        return;
    }
    jit.move(first, a0);
    jit.move(second, a1);
}

// `o.m(..)`: see Typed::Lowering::callMethodThroughSlot() for what stands at the site. thisGPR: the object. calleeGPR: the address of the slot. The arguments are where the call takes them.
// The slot, the object, the arguments: what is on the stack between the two thunks, from the stack pointer up (on x86-64 behind the return address).
static constexpr unsigned bytesSavedForCallMethod(unsigned count) { return WTF::roundUpToMultipleOf<stackAlignmentBytes()>((count + 2) * sizeof(EncodedJSValue)); }

static void generateCallMethod(CCallHelpers& jit, unsigned count, CCallHelpers::Label callValue, CCallHelpers::Label getById)
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
    // What is no function is named by the other thunk.
    otherwise.append(jit.branchIfNotCell(word));
    jit.move(word, calleeGPR);
    jit.jump().linkTo(callValue, &jit);

    otherwise.link(&jit);
    constexpr int32_t toSecondCall = sizeOfJump + sizeOfNearCall;
#if CPU(ARM64)
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    jit.addPtr(TrustedImm32(toSecondCall), CCallHelpers::linkRegister);
    jit.subPtr(TrustedImm32(bytesSavedForCallMethod(count)), sp);
    jit.storePair64(slot, thisGPR, sp, TrustedImm32(0));
    for (unsigned i = 0; i + 1 < count; i += 2)
        jit.storePair64(argumentGPR(i), argumentGPR(i + 1), sp, TrustedImm32((i + 2) * sizeof(EncodedJSValue)));
    if (count & 1)
        jit.store64(argumentGPR(count - 1), Address(sp, (count + 1) * sizeof(EncodedJSValue)));
#else
    constexpr GPRReg returnAddress = word;
    jit.pop(returnAddress);
    jit.addPtr(TrustedImm32(toSecondCall), returnAddress);
    if (count & 1)
        jit.push(returnAddress);
    for (unsigned i = count; i--;)
        jit.push(argumentGPR(i));
    jit.push(thisGPR);
    jit.push(slot);
    jit.push(returnAddress);
#endif
    jit.move(slot, A1);
    jit.move(thisGPR, R0);
    jit.jump().linkTo(getById, &jit);
}

// R0: what Stub::GetById has read.
static void generateCallMethodRead(CCallHelpers& jit, unsigned count, CCallHelpers::Label callValue)
{
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
#if CPU(ARM64)
    Jump isNoCell = jit.branchIfNotCell(R0);
    jit.move(R0, calleeGPR);
    jit.load64(Address(sp, sizeof(EncodedJSValue)), thisGPR);
    for (unsigned i = 0; i + 1 < count; i += 2)
        jit.loadPair64(sp, TrustedImm32((i + 2) * sizeof(EncodedJSValue)), argumentGPR(i), argumentGPR(i + 1));
    if (count & 1)
        jit.load64(Address(sp, (count + 1) * sizeof(EncodedJSValue)), argumentGPR(count - 1));
    jit.addPtr(TrustedImm32(bytesSavedForCallMethod(count)), sp);
    jit.jump().linkTo(callValue, &jit);

    isNoCell.link(&jit);
    jit.loadPtr(Address(sp), A1);
    jit.addPtr(TrustedImm32(bytesSavedForCallMethod(count)), sp);
#else
    constexpr GPRReg returnAddress = T11;
    constexpr GPRReg dropped = T13;
    static_assert(noOverlap(returnAddress, dropped, R0, calleeGPR, A1));
    jit.pop(returnAddress);
    Jump isNoCell = jit.branchIfNotCell(R0);
    jit.move(R0, calleeGPR);
    jit.pop(dropped);
    jit.pop(thisGPR);
    for (unsigned i = 0; i < count; ++i)
        jit.pop(argumentGPR(i));
    if (count & 1)
        jit.pop(dropped);
    jit.push(returnAddress);
    jit.jump().linkTo(callValue, &jit);

    isNoCell.link(&jit);
    jit.pop(A1);
    jit.addPtr(TrustedImm32(bytesSavedForCallMethod(count) - sizeof(EncodedJSValue)), sp);
    jit.push(returnAddress);
#endif
    prepareMissAtSite(jit, Entry::operationAOTTypedThrowMethodIsNotAFunction, 1);
    callAndCheckException(jit, T9, Returns::Void);
}

const StubBlob& stubBlob()
{
    static LazyNeverDestroyed<StubBlob> blob;
    static std::once_flag once;
    std::call_once(once, [] {
        blob.construct();
        CCallHelpers jit;
#if CPU(ARM64) || CPU(X86_64)
        CCallHelpers::Label* labels = stubLabels();
        Vector<std::pair<CCallHelpers::Call, Stub>> callsBetweenStubs;
        s_callsBetweenStubs = &callsBetweenStubs;
        Vector<CCallHelpers::Label> returnsIntoAdapters;
        s_returnsIntoAdapters = &returnsIntoAdapters;
#if CPU(ARM64)
        Vector<LabelAddress> labelAddresses;
        s_labelAddresses = &labelAddresses;
#endif
#else
        Vector<CCallHelpers::Label> returnsIntoAdapters;
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
        static_assert(usesStubs);
        {
            for (Stub stub : operationCallStubs) {
                for (unsigned entry = 0; entry < numberOfEntries; ++entry) {
                    thunkLabels.append(jit.label());
                    jit.move(CCallHelpers::TrustedImm32(entry * sizeof(void*)), T9);
                    jit.jump().linkTo(labels[static_cast<unsigned>(stub)], &jit);
                }
            }
            for (Stub stub : functionCallStubs) {
                for (unsigned count = 0; count < numberOfCountsWithThunk; ++count) {
                    jit.align();
                    thunkLabels.append(jit.label());
                    if (stub == Stub::CallHostFunction || stub == Stub::CallInternalFunction || stub == Stub::ConstructInternalFunction) {
                        callThroughTrampoline(jit, count, stub == Stub::CallHostFunction ? Entry::NativeCallTrampoline : stub == Stub::CallInternalFunction ? Entry::InternalFunctionCallTrampoline : Entry::InternalFunctionConstructTrampoline);
                        continue;
                    }
                    // The usual callee: a closure with the structure it was allocated with, whose entry point takes its arguments in registers, and no more of them than are passed.
                    if (stub == Stub::CallValue) {
                        CCallHelpers::JumpList otherwise;
                        otherwise.append(jit.branchIfNotCell(calleeGPR));
                        jit.load32(Address(calleeGPR, JSCell::structureIDOffset()), T11);
                        otherwise.append(jit.branch32(CCallHelpers::NotEqual, T11, Address(instanceGPR, Instance::offsetOfTypedClosureStructureID())));
                        jit.loadPtr(Address(calleeGPR, Typed::Closure::offsetOfEntry()), T11);
                        enterClosure(jit, count, otherwise);
                        otherwise.link(&jit);
                        jit.jump().linkTo(thunkLabels[firstCallThunkOf(Stub::Call) + count], &jit);
                        continue;
                    }
                    // The site has found the callee to be such a closure, with ClosureWord::constructsOrdinaryObjectBit, and has made `this`. A class is entered like any function.
                    if (stub == Stub::ConstructClosure) {
                        CCallHelpers::JumpList never;
                        jit.loadPtr(Address(calleeGPR, Typed::Closure::offsetOfEntry()), T11);
                        jit.and64(CCallHelpers::TrustedImm64(static_cast<int64_t>(~(1ULL << ClosureWord::isNotCallableBit))), T11);
                        enterClosure(jit, count, never);
                        never.link(&jit);
                        jit.breakpoint();
                        continue;
                    }
                    if (stub == Stub::CallMethod) {
                        generateCallMethod(jit, count, thunkLabels[firstCallThunkOf(Stub::CallValue) + count], labels[static_cast<unsigned>(Stub::GetById)]);
                        continue;
                    }
                    if (stub == Stub::CallMethodRead) {
                        generateCallMethodRead(jit, count, thunkLabels[firstCallThunkOf(Stub::CallValue) + count]);
                        continue;
                    }
                    if (stub == Stub::CallCached) {
                        Jump isAnotherCallee = jit.branchPtr(CCallHelpers::NotEqual, Address(countGPR, OBJECT_OFFSETOF(Slot, pointer)), calleeGPR);
                        jit.farJump(Address(countGPR, sizeof(Slot) + OBJECT_OFFSETOF(Slot, pointer)), JSEntryPtrTag);
                        isAnotherCallee.link(&jit);
                        HostCallThunks thunksToCache { thunkLabels[firstCallThunkOf(Stub::CallHostFunction) + count], thunkLabels[firstCallThunkOf(Stub::CallInternalFunction) + count], thunkLabels.last() };
                        generateCallTo(jit, CodeSpecializationKind::CodeForCall, count, &thunksToCache);
                        continue;
                    }
                    generateCallTo(jit, stub == Stub::Call ? CodeSpecializationKind::CodeForCall : CodeSpecializationKind::CodeForConstruct, count);
                }
            }
            for (unsigned i = 1; i <= numberOfStubIntrinsics; ++i) {
                StubIntrinsic intrinsic = static_cast<StubIntrinsic>(i);
                jit.align();
                thunkLabels.append(jit.label());
                CCallHelpers::JumpList hasAnotherReceiver;
                CCallHelpers::JumpList otherwise;
                CCallHelpers::JumpList needsHostFunction;
                generateCallIntrinsic(jit, intrinsic, hasAnotherReceiver, otherwise, needsHostFunction);
                needsHostFunction.link(&jit);
                jit.add32(TrustedImm32(1), Address(instanceGPR, Instance::offsetOfEffectEpoch()));
                jit.jump().linkTo(thunkLabels[firstCallThunkOf(Stub::CallHostFunction) + argumentCountOf(intrinsic)], &jit);
                otherwise.link(&jit);
                jit.add32(TrustedImm32(1), Address(instanceGPR, Instance::offsetOfEffectEpoch()));
                jit.jump().linkTo(thunkLabels[firstCallThunkOf(Stub::Call) + argumentCountOf(intrinsic)], &jit);
                hasAnotherReceiver.link(&jit);
                jit.add32(TrustedImm32(1), Address(instanceGPR, Instance::offsetOfEffectEpoch()));
                jit.jump().linkTo(thunkLabels[firstCallThunkOf(Stub::CallCached) + argumentCountOf(intrinsic)], &jit);
            }
#if CPU(ARM64)
            static_assert(!static_cast<unsigned>(ARM64Registers::x0));
            for (Stub stub : stubsWithAnyRegisterOperand) {
                for (unsigned number = 0; number < numberOfRegistersForOperand; ++number) {
                    GPRReg reg = static_cast<GPRReg>(number);
                    if (!operandAllowedInRegister(stub, reg) || reg == defaultOperandRegister(stub)) {
                        thunkLabels.append(labels[static_cast<unsigned>(stub)]);
                        continue;
                    }
                    jit.align();
                    thunkLabels.append(jit.label());
                    switch (stub) {
                    case Stub::WriteBarrier:
                        generateWriteBarrier(jit, reg);
                        break;
                    case Stub::ToBoolean:
                        generateToBoolean(jit, reg);
                        break;
                    case Stub::GetById:
                        generateGetByIdFrom(jit, Entry::operationAOTGetById, reg);
                        break;
                    case Stub::GetByIdWellKnown:
                        generateGetByIdFrom(jit, Entry::operationAOTGetByIdWellKnown, reg);
                        break;
                    default:
                        if (auto read = slotReadBy(stub)) {
                            generateReadSlot(jit, read->first, read->second, reg);
                            break;
                        }
                        jit.move(reg, defaultOperandRegister(stub));
                        jit.jump().linkTo(labels[static_cast<unsigned>(stub)], &jit);
                        break;
                    }
                }
            }
            for (auto& [stub, operation] : operationsWithAnyRegisterOperand) {
                for (unsigned number = 0; number < numberOfRegistersForOperand; ++number) {
                    if (!operandAllowedInRegister(stub, static_cast<GPRReg>(number))) {
                        thunkLabels.append(labels[static_cast<unsigned>(stub)]);
                        continue;
                    }
                    thunkLabels.append(jit.label());
                    jit.move(static_cast<GPRReg>(number), defaultOperandRegister(stub));
                    jit.move(CCallHelpers::TrustedImm32(static_cast<unsigned>(operation) * sizeof(void*)), GPRInfo::regT9);
                    jit.jump().linkTo(labels[static_cast<unsigned>(stub)], &jit);
                }
            }
            for (Stub stub : stubsWithTwoAnyRegisterOperands) {
                for (unsigned i = 0; i < numberOfRegistersForPair; ++i) {
                    for (unsigned j = 0; j < numberOfRegistersForPair; ++j) {
                        thunkLabels.append(jit.label());
                        moveToFirstTwoArguments(jit, registerForPair(i), registerForPair(j));
                        jit.jump().linkTo(labels[static_cast<unsigned>(stub)], &jit);
                    }
                }
            }
            CCallHelpers::Label resultRegisterEntrypoints[2][numberOfRegistersForResult];
            for (bool suppliesInstance : { false, true }) {
                for (unsigned i = 0; i < numberOfRegistersForResult; ++i) {
                    jit.align();
                    resultRegisterEntrypoints[suppliesInstance][i] = jit.label();
                    generateOperation(jit, Returns::Value, suppliesInstance ? Supplies::Instance : Supplies::GlobalObject, static_cast<GPRReg>(static_cast<unsigned>(ARM64Registers::x19) + i));
                }
            }
            for (auto& [operation, acceptsOperandInAnyRegister] : operationsWithAnyRegisterResult) {
                for (unsigned i = 0; i < numberOfRegistersForResult; ++i) {
                    for (unsigned number = 0; number < numberOfRegistersForOperand; ++number) {
                        if ((!acceptsOperandInAnyRegister && number) || !operandAllowedInRegister(Stub::OperationValueWithGlobalObject, static_cast<GPRReg>(number))) {
                            thunkLabels.append(thunkLabels.last());
                            continue;
                        }
                        thunkLabels.append(jit.label());
                        if (acceptsOperandInAnyRegister)
                            jit.move(static_cast<GPRReg>(number), GPRInfo::argumentGPR1);
                        jit.move(CCallHelpers::TrustedImm32(static_cast<unsigned>(operation) * sizeof(void*)), GPRInfo::regT9);
                        jit.jump().linkTo(resultRegisterEntrypoints[takesInstance(operation)][i], &jit);
                    }
                }
            }
            RELEASE_ASSERT(thunkLabels.size() == numberOfThunks);
#endif
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
            auto entryName = [&](Entry entry) { return entryNames[static_cast<unsigned>(entry)]; };
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
            for (unsigned i = 1; i <= numberOfStubIntrinsics && !blob->thunkOffsets.isEmpty(); ++i)
                note(blob->thunkOffsets[thunk++], "Intrinsic"_s, i);
            for (Stub stub : stubsWithAnyRegisterOperand) {
                if (hasNoPerRegisterEntrypoints())
                    break;
                for (unsigned number = 0; number < numberOfRegistersForOperand; ++number, ++thunk) {
                    if (blob->thunkOffsets[thunk] != blob->offsets[static_cast<unsigned>(stub)])
                        note(blob->thunkOffsets[thunk], nameOf(stub), " of x"_s, number);
                }
            }
            for (auto& [stub, operation] : operationsWithAnyRegisterOperand) {
                if (hasNoPerRegisterEntrypoints())
                    break;
                for (unsigned number = 0; number < numberOfRegistersForOperand; ++number)
                    note(blob->thunkOffsets[thunk++], nameOf(stub), " to "_s, entryName(operation), " of x"_s, number);
            }
            for (Stub stub : stubsWithTwoAnyRegisterOperands) {
                if (hasNoPerRegisterEntrypoints())
                    break;
                for (unsigned i = 0; i < numberOfRegistersForPair * numberOfRegistersForPair; ++i)
                    note(blob->thunkOffsets[thunk++], nameOf(stub), " of pair "_s, i);
            }
            for (auto& [operation, acceptsOperandInAnyRegister] : operationsWithAnyRegisterResult) {
                if (hasNoPerRegisterEntrypoints())
                    break;
                for (unsigned i = 0; i < numberOfRegistersForResult * numberOfRegistersForOperand; ++i, ++thunk) {
                    if (acceptsOperandInAnyRegister || !(i % numberOfRegistersForOperand))
                        note(blob->thunkOffsets[thunk], "OperationValue to "_s, entryName(operation), " of x"_s, i % numberOfRegistersForOperand, " into x"_s, 19 + i / numberOfRegistersForOperand);
                }
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
    if (!thunk)
        jit.move(CCallHelpers::TrustedImm32(t9Value), stubImmediateGPR);
    m_pending.append({ jit.nearCall(), stub, false, site.bits });
    if (thunk)
        m_pending.last().thunk = safeCast<uint16_t>(*thunk + 1);
}

void StubCalls::tailCall(CCallHelpers& jit, Stub stub)
{
    m_pending.append({ jit.nearTailCall(), stub, true, StubCall::noCallSite });
}

void StubCalls::tailCall(CCallHelpers& jit, Stub stub, uint32_t t9Value)
{
    auto thunk = thunkFor(stub, t9Value);
    if (!thunk)
        jit.move(CCallHelpers::TrustedImm32(t9Value), stubImmediateGPR);
    m_pending.append({ jit.nearTailCall(), stub, true, StubCall::noCallSite });
    if (thunk)
        m_pending.last().thunk = safeCast<uint16_t>(*thunk + 1);
}

void StubCalls::callFunction(CCallHelpers& jit, uint32_t knownCallee, CallSite site)
{
    m_pending.append({ jit.nearCall(), Stub::Call, false, site.bits, knownCallee });
}

void StubCalls::jumpToFunction(CCallHelpers& jit, uint32_t knownCallee)
{
    m_pending.append({ jit.nearTailCall(), Stub::Call, true, StubCall::noCallSite, knownCallee });
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
    jit.m_assembler.movl_mr_disp32(0, base, dest);
    m_references.append({ jit.label(), addend, scale });
    jit.zeroExtend16To32(dest, dest);
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
    RELEASE_ASSERT(!encoded);
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
        result.append({ static_cast<uint32_t>(location - start - sizeBeforeLocation), pending.stub, pending.isTailCall, pending.thunk, pending.function, pending.callSite });
    }
    return result;
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
