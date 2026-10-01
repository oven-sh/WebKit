/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubs.h"

#if ENABLE(FTL_JIT)

#include "AOTEmitter.h"
#include "AOTImage.h"
#include "AOTRuntime.h"
#include "AOTThunks.h"
#include "BaselineJITRegisters.h"
#include "CodeBlock.h"
#include "FunctionExecutable.h"
#include "GetterSetter.h"
#include "Interpreter.h"
#include "JSGlobalObject.h"
#include "JSMap.h"
#include "JSSet.h"
#include "JSCInlines.h"
#include "JSWebAssemblyInstance.h"
#include "LinkBuffer.h"
#include "MaxFrameExtentForSlowPathCall.h"
#include "ThunkGenerators.h"
#include "VM.h"
#include <wtf/NeverDestroyed.h>

namespace JSC { namespace AOT {

using Jump = CCallHelpers::Jump;
using Address = CCallHelpers::Address;
using TrustedImm32 = CCallHelpers::TrustedImm32;

#if CPU(ARM64)

// State used while generating the stubs: the offsets of the stubs generated so far, and the calls between stubs, which are linked
// at the end.
static CCallHelpers::Label s_labels[numberOfStubs];
static Vector<std::pair<CCallHelpers::Call, Stub>>* s_callsBetweenStubs;
static Vector<CCallHelpers::Label>* s_returnsIntoAdapters;
// Places where the address of a location in the stubs must be materialized in a register.
struct AddressOfLabel {
    CCallHelpers::Label instruction;
    GPRReg reg;
    CCallHelpers::Label target;
};
static Vector<AddressOfLabel>* s_addressesOfLabels;
static CCallHelpers::Label s_callVarargsReturnAddress;
static CCallHelpers::Label s_whereCallVarargsIsReturnedTo;

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

constexpr GPRReg A0 = GPRInfo::argumentGPR0;
constexpr GPRReg A1 = GPRInfo::argumentGPR1;
constexpr GPRReg A2 = GPRInfo::argumentGPR2;
constexpr GPRReg A3 = GPRInfo::argumentGPR3;
constexpr GPRReg A4 = GPRInfo::argumentGPR4;
constexpr GPRReg A5 = GPRInfo::argumentGPR5;
constexpr GPRReg T9 = GPRInfo::regT9;
constexpr GPRReg T10 = GPRInfo::regT10;
constexpr GPRReg T11 = GPRInfo::regT11;
constexpr GPRReg T12 = GPRInfo::regT12;
constexpr GPRReg T13 = GPRInfo::regT13;
constexpr GPRReg T14 = ARM64Registers::x14;
constexpr GPRReg T15 = ARM64Registers::x15;
static_assert(noOverlap(thisGPR, countGPR, calleeGPR, T11, T12, T13, T14, T15) && countGPR == T9 && calleeGPR == T10);

// Preserves all argument registers.
static void generatePrologue(CCallHelpers& jit)
{
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfVM()), T11);
    jit.subPtr(GPRInfo::callFrameRegister, T9, T12);
    jit.loadPtr(Address(T11, VM::offsetOfSoftStackLimit()), T11);
    Jump overflow = jit.branchPtr(CCallHelpers::Above, T11, T12);
    // The frame is so large that the subtraction wrapped around.
    Jump wrapped = jit.branchPtr(CCallHelpers::Above, T12, GPRInfo::callFrameRegister);
    jit.move(T12, CCallHelpers::stackPointerRegister);
    jit.ret();

    overflow.link(&jit);
    wrapped.link(&jit);
    jit.move(instanceGPR, T11);
    jumpToEntry(jit, T11, Entry::ThrowStackOverflowAtPrologue);
}

static void loadInstance(CCallHelpers& jit, GPRReg result)
{
    jit.move(instanceGPR, result);
}

// Loads into T15 the index of the function that contains `pc`. A stub identifies its caller from its return address. Only reads two
// of the image's tables, at entries adjacent to those for neighboring code. Clobbers T14.
void loadIndexOfFunctionAt(CCallHelpers& jit, GPRReg pc)
{
    static_assert(indexOfFunctionGPR == T15);
    ASSERT(noOverlap(pc, T14, T15));
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfCode()), T14);
    jit.subPtr(pc, T14, T14);
    jit.urshiftPtr(T14, TrustedImm32(shiftOfGranuleOfCode), T15);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfGranulesOfCode()), CCallHelpers::memoryTempRegister);
    jit.load32(CCallHelpers::BaseIndex(CCallHelpers::memoryTempRegister, T15, CCallHelpers::TimesFour), T15);
    // That is the last function that starts at or before the granule. It may end before `pc`.
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfStartsOfFunctionsAfterFirst()), CCallHelpers::memoryTempRegister);
    CCallHelpers::Label next = jit.label();
    jit.load32(CCallHelpers::BaseIndex(CCallHelpers::memoryTempRegister, T15, CCallHelpers::TimesFour), CCallHelpers::dataTempRegister);
    Jump found = jit.branch32(CCallHelpers::Above, CCallHelpers::dataTempRegister, T14);
    jit.add32(TrustedImm32(1), T15);
    jit.jump().linkTo(next, &jit);
    found.link(&jit);
}

// For a stub that has not made a call yet.
static void loadIndexOfCaller(CCallHelpers& jit)
{
    loadIndexOfFunctionAt(jit, CCallHelpers::linkRegister);
}

// Loads the FunctionInfo of the function whose index is in T15, given its Instance.
static void loadInfo(CCallHelpers& jit, GPRReg instance, GPRReg result)
{
    ASSERT(result != instance && result != T15);
    static_assert(sizeof(FunctionInfo) == 32);
    jit.lshiftPtr(T15, TrustedImm32(5), result);
    jit.loadPtr(Address(instance, Instance::offsetOfInfos()), CCallHelpers::memoryTempRegister);
    jit.addPtr(CCallHelpers::memoryTempRegister, result);
}

// Loads the Data that contains the slot. It belongs to the function whose index is in T15: either the function's own Data or, if it
// had none when it was called, SharedData. Leaves the Instance in `instance`.
static void loadInstanceAndDataOfSlot(CCallHelpers& jit, GPRReg slot, GPRReg instance, GPRReg data)
{
    ASSERT(noOverlap(slot, instance, data, T15));
    loadInstance(jit, instance);
    jit.loadPtr(Address(instance, Instance::offsetOfSharedData()), data);
    jit.subPtr(slot, data, CCallHelpers::memoryTempRegister);
    Jump isShared = jit.branchPtr(CCallHelpers::Below, CCallHelpers::memoryTempRegister, CCallHelpers::TrustedImmPtr(SharedData::size));
    jit.lshiftPtr(T15, TrustedImm32(2), data);
    jit.addPtr(instance, data);
    jit.load32(Address(data, Instance::offsetOfStates()), data);
    jit.lshiftPtr(TrustedImm32(Instance::shiftOfStateWithData), data);
    jit.addPtr(instance, data);
    isShared.link(&jit);
}

// Call after the above, on a cache miss. If the slot is in SharedData, counts the miss. Once a function has taken enough misses
// (Instance::missesToPutUpWithFor()) it gets its own Data, which takes effect the next time it is called. Preserves everything but
// T11-T13. This is for misses that may still be resolved without calling an operation; operations count their own misses.
static void countMissOfSlot(CCallHelpers& jit, GPRReg instance, GPRReg data)
{
    ASSERT(instance == T9 && data == T10);
    jit.loadPtr(Address(instance, Instance::offsetOfSharedData()), T11);
    Jump isItsOwn = jit.branchPtr(CCallHelpers::NotEqual, data, T11);
    jit.move(T15, T11);
    jit.addPtr(TrustedImm32(Instance::offsetOfStates()), instance, T12);
    jit.load32(CCallHelpers::BaseIndex(T12, T11, CCallHelpers::TimesFour), T13);
    // The function was given its own Data after this call began and keeps using SharedData until it returns. The state word is no
    // longer a count.
    Jump hasItsOwnByNow = jit.branch32(CCallHelpers::AboveOrEqual, T13, TrustedImm32(Instance::leastStateWithData));
    jit.add32(TrustedImm32(1), T13);
    // The count is in the low 16 bits.
    jit.store16(T13, CCallHelpers::BaseIndex(T12, T11, CCallHelpers::TimesFour));
    jit.zeroExtend16To32(T13, T13);
    static_assert(sizeof(FunctionInfo) == 32);
    jit.lshiftPtr(T11, TrustedImm32(5), T12);
    jit.loadPtr(Address(instance, Instance::offsetOfInfos()), CCallHelpers::memoryTempRegister);
    jit.addPtr(CCallHelpers::memoryTempRegister, T12);
    // Inline version of Instance::missesToPutUpWithFor(). The saturated case cannot occur, because a function with that many slots
    // does not start cold.
    jit.load16(Address(T12, FunctionInfo::offsetOfFlags()), T12);
    jit.urshift32(TrustedImm32(FunctionInfo::numberOfFlagBits), T12);
    jit.load32(Address(instance, Instance::offsetOfMissesForEightSlots()), CCallHelpers::memoryTempRegister);
    jit.mul32(CCallHelpers::memoryTempRegister, T12, T12);
    jit.urshift32(TrustedImm32(3), T12);
    jit.load32(Address(instance, Instance::offsetOfMissesToSpare()), CCallHelpers::memoryTempRegister);
    jit.add32(CCallHelpers::memoryTempRegister, T12);
    // Only when the count first reaches the limit. Calls already in progress keep counting.
    Jump notYet = jit.branch32(CCallHelpers::NotEqual, T13, T12);
    jit.subPtr(TrustedImm32(96), CCallHelpers::stackPointerRegister);
    jit.storePair64(A0, A1, CCallHelpers::stackPointerRegister, TrustedImm32(0));
    jit.storePair64(A2, A3, CCallHelpers::stackPointerRegister, TrustedImm32(16));
    jit.storePair64(A4, A5, CCallHelpers::stackPointerRegister, TrustedImm32(32));
    jit.storePair64(GPRInfo::argumentGPR6, CCallHelpers::linkRegister, CCallHelpers::stackPointerRegister, TrustedImm32(48));
    jit.storePair64(instance, data, CCallHelpers::stackPointerRegister, TrustedImm32(64));
    jit.storePtr(T15, Address(CCallHelpers::stackPointerRegister, 80));
    jit.move(instance, A0);
    jit.move(T11, A1);
    jit.loadPtr(Address(instance, Instance::offsetOfRuntimeTable()), T12);
    jit.loadPtr(Address(T12, static_cast<unsigned>(Entry::operationAOTGiveData) * sizeof(void*)), T12);
    jit.call(T12, OperationPtrTag);
    jit.loadPair64(CCallHelpers::stackPointerRegister, TrustedImm32(0), A0, A1);
    jit.loadPair64(CCallHelpers::stackPointerRegister, TrustedImm32(16), A2, A3);
    jit.loadPair64(CCallHelpers::stackPointerRegister, TrustedImm32(32), A4, A5);
    jit.loadPair64(CCallHelpers::stackPointerRegister, TrustedImm32(48), GPRInfo::argumentGPR6, CCallHelpers::linkRegister);
    jit.loadPair64(CCallHelpers::stackPointerRegister, TrustedImm32(64), instance, data);
    jit.loadPtr(Address(CCallHelpers::stackPointerRegister, 80), T15);
    jit.addPtr(TrustedImm32(96), CCallHelpers::stackPointerRegister);
    notYet.link(&jit);
    hasItsOwnByNow.link(&jit);
    isItsOwn.link(&jit);
}

enum class Returns : uint8_t { Value, Void, Double };

// The target is in `function` and its arguments are in place. The stub sets up its own frame for the duration of the call, like
// every stub that calls something that may walk the stack. Its return address identifies the calling function's current location
// (see frameAt()).
static void callAndCheckException(CCallHelpers& jit, GPRReg function, Returns returns, GPRReg result = GPRInfo::returnValueGPR)
{
    jit.emitFunctionPrologue();
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

static void generateOperation(CCallHelpers& jit, Returns returns, bool withGlobalObject, GPRReg result = GPRInfo::returnValueGPR)
{
    loadInstance(jit, T11);
    if (withGlobalObject)
        jit.loadPtr(Address(T11, Instance::offsetOfGlobalObject()), A0);
    jit.loadPtr(Address(T11, Instance::offsetOfRuntimeTable()), T11);
    jit.loadPtr(CCallHelpers::BaseIndex(T11, T9, CCallHelpers::TimesOne), T11);
    callAndCheckException(jit, T11, returns, result);
}

static void generateColdOperation(CCallHelpers& jit, bool ofLeaf, bool returnsValue = false)
{
    constexpr GPRReg fp = GPRInfo::callFrameRegister;
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    constexpr GPRReg scratch = ARM64Registers::x16;
    if (ofLeaf) {
        // Do what its prologue would have done.
        jit.pushPair(fp, T10);
        jit.move(sp, fp);
    }
    jit.emitFunctionPrologue();
    constexpr unsigned numberOfGPRs = 16; // x0 to x15
    constexpr unsigned numberOfFPRs = 24; // d0 to d7, d16 to d31
    auto fpr = [](unsigned i) { return static_cast<FPRReg>(i < 8 ? ARM64Registers::q0 + i : ARM64Registers::q16 + (i - 8)); };
    jit.subPtr(TrustedImm32((numberOfGPRs + numberOfFPRs) * 8), sp);
    for (unsigned i = 0; i < numberOfGPRs; i += 2)
        jit.storePair64(static_cast<GPRReg>(ARM64Registers::x0 + i), static_cast<GPRReg>(ARM64Registers::x0 + i + 1), sp, TrustedImm32(i * 8));
    for (unsigned i = 0; i < numberOfFPRs; ++i)
        jit.storeDouble(fpr(i), Address(sp, (numberOfGPRs + i) * 8));
    loadInstance(jit, T11);
    jit.loadPtr(Address(T11, Instance::offsetOfGlobalObject()), A0);
    jit.loadPtr(Address(T11, Instance::offsetOfRuntimeTable()), T11);
    jit.loadPtr(CCallHelpers::BaseIndex(T11, T9, CCallHelpers::TimesOne), T11);
    jit.call(T11, OperationPtrTag);
    jit.move(returnsValue ? GPRInfo::returnValueGPR2 : GPRInfo::returnValueGPR, scratch);
    if (returnsValue)
        jit.load64(Address(sp, 8), ARM64Registers::x1);
    for (unsigned i = returnsValue ? 2 : 0; i < numberOfGPRs; i += 2)
        jit.loadPair64(sp, TrustedImm32(i * 8), static_cast<GPRReg>(ARM64Registers::x0 + i), static_cast<GPRReg>(ARM64Registers::x0 + i + 1));
    for (unsigned i = 0; i < numberOfFPRs; ++i)
        jit.loadDouble(Address(sp, (numberOfGPRs + i) * 8), fpr(i));
    jit.emitFunctionEpilogue();
    Jump exception = jit.branchTestPtr(CCallHelpers::NonZero, scratch);
    if (ofLeaf) {
        jit.move(CCallHelpers::linkRegister, scratch);
        jit.popPair(fp, CCallHelpers::linkRegister);
        jit.farJump(scratch, NoPtrTag);
    } else
        jit.ret();
    exception.link(&jit);
    loadInstance(jit, T9);
    jumpToEntry(jit, T9, Entry::HandleException);
}
static void generateColdOperationVoid(CCallHelpers& jit) { generateColdOperation(jit, false); }
static void generateColdOperationVoidOfLeaf(CCallHelpers& jit) { generateColdOperation(jit, true); }
static void generateColdOperationValue(CCallHelpers& jit) { generateColdOperation(jit, false, true); }
static void generateColdOperationValueOfLeaf(CCallHelpers& jit) { generateColdOperation(jit, true, true); }

static void generateOperationValue(CCallHelpers& jit) { generateOperation(jit, Returns::Value, false); }
static void generateOperationVoid(CCallHelpers& jit) { generateOperation(jit, Returns::Void, false); }
static void generateOperationDouble(CCallHelpers& jit) { generateOperation(jit, Returns::Double, false); }
static void generateOperationValueWithGlobalObject(CCallHelpers& jit) { generateOperation(jit, Returns::Value, true); }
static void generateOperationVoidWithGlobalObject(CCallHelpers& jit) { generateOperation(jit, Returns::Void, true); }
static void generateOperationDoubleWithGlobalObject(CCallHelpers& jit) { generateOperation(jit, Returns::Double, true); }

static void generatePlain(CCallHelpers& jit, std::optional<ptrdiff_t> firstArgument)
{
    loadInstance(jit, T11);
    if (firstArgument)
        jit.loadPtr(Address(T11, *firstArgument), A0);
    jit.loadPtr(Address(T11, Instance::offsetOfRuntimeTable()), T11);
    jit.loadPtr(CCallHelpers::BaseIndex(T11, T9, CCallHelpers::TimesOne), T11);
    jit.farJump(T11, OperationPtrTag);
}

static void generatePlainOperation(CCallHelpers& jit) { generatePlain(jit, std::nullopt); }
static void generatePlainOperationWithGlobalObject(CCallHelpers& jit) { generatePlain(jit, Instance::offsetOfGlobalObject()); }
static void generatePlainOperationWithVM(CCallHelpers& jit) { generatePlain(jit, Instance::offsetOfVM()); }

// Calls an operation that neither throws nor walks the stack, then returns from the stub with every register preserved (except the
// return value register, if the result is used). For rare cases, so that callers of the stub need not spill anything. setUp places
// the arguments. It receives the Data in T10 and must preserve it.
template<typename SetUp>
static void callPreservingRegistersAndReturn(CCallHelpers& jit, Entry operation, bool hasResult, const SetUp& setUp)
{
    RegisterSet toSave = RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters());
    Vector<Reg, 48> registers;
    toSave.forEach([&](Reg reg) {
        if (!hasResult || reg != Reg(GPRInfo::returnValueGPR))
            registers.append(reg);
    });
    unsigned bytes = WTF::roundUpToMultipleOf<stackAlignmentBytes()>((registers.size() + 1) * sizeof(CPURegister));
    jit.subPtr(TrustedImm32(bytes), CCallHelpers::stackPointerRegister);
    jit.storePtr(CCallHelpers::linkRegister, Address(CCallHelpers::stackPointerRegister));
    for (unsigned i = 0; i < registers.size(); ++i) {
        Address address(CCallHelpers::stackPointerRegister, (i + 1) * sizeof(CPURegister));
        if (registers[i].isGPR())
            jit.storePtr(registers[i].gpr(), address);
        else
            jit.storeDouble(registers[i].fpr(), address);
    }
    loadInstance(jit, T10);
    setUp();
    jit.loadPtr(Address(T10, Instance::offsetOfRuntimeTable()), T10);
    jit.loadPtr(Address(T10, static_cast<unsigned>(operation) * sizeof(void*)), T10);
    jit.call(T10, OperationPtrTag);
    for (unsigned i = 0; i < registers.size(); ++i) {
        Address address(CCallHelpers::stackPointerRegister, (i + 1) * sizeof(CPURegister));
        if (registers[i].isGPR())
            jit.loadPtr(address, registers[i].gpr());
        else
            jit.loadDouble(address, registers[i].fpr());
    }
    jit.loadPtr(Address(CCallHelpers::stackPointerRegister), CCallHelpers::linkRegister);
    jit.addPtr(TrustedImm32(bytes), CCallHelpers::stackPointerRegister);
    jit.ret();
}

// The slow path of a stub with acceptsOperandInAnyRegister() is shared by all its entry points: by then the operand is in T9. The
// entry point for the default register is generated first and contains the slow path.
static CCallHelpers::Label s_longWayOfWriteBarrier;
static CCallHelpers::Label s_longWayOfToBoolean;

static void generateWriteBarrier(CCallHelpers& jit, GPRReg owner)
{
    jit.load8(Address(owner, JSCell::cellStateOffset()), T9);
    loadInstance(jit, T10);
    jit.loadPtr(Address(T10, Instance::offsetOfVM()), T11);
    jit.load32(Address(T11, VM::offsetOfHeapBarrierThreshold()), T11);
    Jump slow = jit.branch32(CCallHelpers::BelowOrEqual, T9, T11);
    jit.ret();

    slow.link(&jit);
    jit.move(owner, T9);
    if (owner != A0) {
        jit.jump().linkTo(s_longWayOfWriteBarrier, &jit);
        return;
    }
    s_longWayOfWriteBarrier = jit.label();
    callPreservingRegistersAndReturn(jit, Entry::operationAOTWriteBarrier, false, [&] {
        jit.move(T9, A1);
        jit.loadPtr(Address(T10, Instance::offsetOfVM()), A0);
    });
}
static void generateWriteBarrier(CCallHelpers& jit) { generateWriteBarrier(jit, A0); }

// The result is in A0 regardless of which register held the operand.
static void generateToBoolean(CCallHelpers& jit, GPRReg asked)
{
    auto answer = [&](bool value) {
        jit.move(TrustedImm32(value), A0);
        jit.ret();
    };

    // false and true differ only in the low bit.
    jit.xor64(TrustedImm32(JSValue::ValueFalse), asked, T9);
    Jump notBoolean = jit.branchTest64(CCallHelpers::NonZero, T9, TrustedImm32(~1));
    jit.move(T9, A0);
    jit.ret();

    notBoolean.link(&jit);
    jit.move(CCallHelpers::TrustedImm64(JSValue::NumberTag), T10);
    Jump notInt32 = jit.branch64(CCallHelpers::Below, asked, T10);
    jit.test32(CCallHelpers::NonZero, asked, asked, A0);
    jit.ret();

    notInt32.link(&jit);
    Jump notNumber = jit.branchTest64(CCallHelpers::Zero, asked, T10);
    // A double is falsy if it is zero or NaN, which can be determined from its bits with the sign cleared.
    jit.add64(T10, asked, T9);
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
    answer(false); // undefined, null.

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
    jit.test32(CCallHelpers::NonZero, T9, T9, A0);
    jit.ret();

    slow.link(&jit);
    jit.move(asked, T9);
    if (asked != A0) {
        jit.jump().linkTo(s_longWayOfToBoolean, &jit);
        return;
    }
    s_longWayOfToBoolean = jit.label();
    callPreservingRegistersAndReturn(jit, Entry::operationAOTToBoolean, true, [&] {
        jit.move(T9, A1);
        jit.loadPtr(Address(T10, Instance::offsetOfGlobalObject()), A0);
    });
}
static void generateToBoolean(CCallHelpers& jit) { generateToBoolean(jit, A0); }

static void generateEqual(CCallHelpers& jit, Entry operation)
{
    jit.move(CCallHelpers::TrustedImm64(JSValue::NumberTag), T9);
    Jump notBothInt32 = jit.branch64(CCallHelpers::Below, A0, T9);
    Jump notBothInt32Either = jit.branch64(CCallHelpers::Below, A1, T9);
    jit.compare32(CCallHelpers::Equal, A0, A1, A0);
    jit.ret();

    notBothInt32.link(&jit);
    notBothInt32Either.link(&jit);
    // Identical bits and not a number: equal, whatever the value is.
    Jump differ = jit.branch64(CCallHelpers::NotEqual, A0, A1);
    Jump isNumber = jit.branchTest64(CCallHelpers::NonZero, A0, T9);
    jit.move(TrustedImm32(1), A0);
    jit.ret();

    differ.link(&jit);
    isNumber.link(&jit);
    loadInstance(jit, T11);
    jit.move(A1, A2);
    jit.move(A0, A1);
    jit.loadPtr(Address(T11, Instance::offsetOfGlobalObject()), A0);
    jit.loadPtr(Address(T11, Instance::offsetOfRuntimeTable()), T11);
    jit.loadPtr(Address(T11, static_cast<unsigned>(operation) * sizeof(void*)), T11);
    callAndCheckException(jit, T11, Returns::Value);
}

static void generateStrictEqual(CCallHelpers& jit) { generateEqual(jit, Entry::operationAOTCompareStrictEq); }

// The prototype walk of AccessCase::InstanceOfMegamorphic.
static void generateInstanceOf(CCallHelpers& jit)
{
    CCallHelpers::JumpList slowPath;
    slowPath.append(jit.branchIfNotCell(A1));
    slowPath.append(jit.branchIfNotObject(A1));
    jit.move(A0, T11);

    CCallHelpers::Label loop = jit.label();
    // AssemblyHelpers::emitLoadPrototype(), with the StructureID decoded position-independently.
    slowPath.append(jit.branchTest8(CCallHelpers::NonZero, Address(T11, JSObject::typeInfoFlagsOffset()), TrustedImm32(OverridesGetPrototype)));
    jit.load32(Address(T11, JSCell::structureIDOffset()), T12);
    jit.or64(CCallHelpers::TrustedImm64(structureIDBaseOfImages), T12);
    jit.load64(Address(T12, Structure::prototypeOffset()), T12);
    Jump hasMonoProto = jit.branchTest64(CCallHelpers::NonZero, T12);
    jit.load64(Address(T11, offsetRelativeToBase(knownPolyProtoOffset)), T12);
    hasMonoProto.link(&jit);
    Jump isInstance = jit.branch64(CCallHelpers::Equal, T12, A1);
    jit.move(T12, T11);
    jit.branchIfCell(T11).linkTo(loop, &jit);
    jit.move(TrustedImm32(0), A0);
    jit.ret();

    isInstance.link(&jit);
    jit.move(TrustedImm32(1), A0);
    jit.ret();

    slowPath.link(&jit);
    loadInstance(jit, T11);
    jit.move(A1, A2);
    jit.move(A0, A1);
    jit.loadPtr(Address(T11, Instance::offsetOfGlobalObject()), A0);
    jit.loadPtr(Address(T11, Instance::offsetOfRuntimeTable()), T11);
    jit.loadPtr(Address(T11, static_cast<unsigned>(Entry::operationAOTDefaultHasInstance) * sizeof(void*)), T11);
    callAndCheckException(jit, T11, Returns::Value);
}

// For the string in A0: returns the address of its characters in the low 48 bits and its length (saturated at 0xffff) above them.
// Returns a null address for a rope or a 16-bit string. A substring of an 8-bit string is read in place. Clobbers only T9-T11.
static void generateNarrowCharacters(CCallHelpers& jit)
{
    CCallHelpers::JumpList isNotForTheLooking;
    jit.loadPtr(Address(A0, JSString::offsetOfValue()), T9);
    Jump isRope = jit.branchIfRopeStringImpl(T9);
    jit.load32(Address(T9, StringImpl::lengthMemoryOffset()), T10);
    jit.load32(Address(T9, StringImpl::flagsOffset()), T11);
    isNotForTheLooking.append(jit.branchTest32(CCallHelpers::Zero, T11, TrustedImm32(StringImpl::flagIs8Bit())));
    jit.loadPtr(Address(T9, StringImpl::dataOffset()), A0);

    CCallHelpers::Label pack = jit.label();
    Jump fits = jit.branch32(CCallHelpers::BelowOrEqual, T10, TrustedImm32(0xffff));
    jit.move(TrustedImm32(0xffff), T10);
    fits.link(&jit);
    jit.lshift64(TrustedImm32(48), T10);
    jit.or64(T10, A0);
    jit.ret();

    isRope.link(&jit);
    jit.load32(Address(A0, JSRopeString::offsetOfLength()), T10);
    constexpr uintptr_t narrowSlice = JSRopeString::isSubstringInPointer | JSRopeString::is8BitInPointer;
    jit.and64(TrustedImm32(narrowSlice), T9, T11);
    isNotForTheLooking.append(jit.branch64(CCallHelpers::NotEqual, T11, TrustedImm32(narrowSlice)));
    // See JSRopeString::CompactFibers. The base string of a substring is always resolved.
    jit.load64(Address(A0, JSRopeString::offsetOfFiber1()), T9);
    jit.load64(Address(A0, JSRopeString::offsetOfFiber2()), T11);
    jit.urshift64(TrustedImm32(32), T9);
    jit.and64(TrustedImm32(0xffff), T11, A0);
    jit.lshift64(TrustedImm32(32), A0);
    jit.or64(T9, A0);
    jit.urshift64(TrustedImm32(16), T11);
    jit.loadPtr(Address(A0, JSString::offsetOfValue()), A0);
    jit.loadPtr(Address(A0, StringImpl::dataOffset()), A0);
    jit.add64(T11, A0);
    jit.jump().linkTo(pack, &jit);

    isNotForTheLooking.link(&jit);
    jit.move(TrustedImm32(0), A0);
    jit.jump().linkTo(pack, &jit);
}

// A0 === A1, where A1 is a string literal: resolved, 8-bit and an atom.
static void generateIsStringEqualTo(CCallHelpers& jit)
{
    CCallHelpers::JumpList isTrue;
    CCallHelpers::JumpList isFalse;
    CCallHelpers::JumpList slow;
    isTrue.append(jit.branch64(CCallHelpers::Equal, A0, A1));
    isFalse.append(jit.branchIfNotCell(A0));
    jit.load8(Address(A0, JSCell::typeInfoTypeOffset()), T9);
    isFalse.append(jit.branch32(CCallHelpers::NotEqual, T9, TrustedImm32(StringType)));
    jit.loadPtr(Address(A0, JSString::offsetOfValue()), A2);
    jit.loadPtr(Address(A1, JSString::offsetOfValue()), A3);
    jit.load32(Address(A3, StringImpl::lengthMemoryOffset()), A4);
    Jump isRope = jit.branchIfRopeStringImpl(A2);
    isTrue.append(jit.branchPtr(CCallHelpers::Equal, A2, A3));
    // The lengths usually differ, which settles it.
    jit.load32(Address(A2, StringImpl::lengthMemoryOffset()), T9);
    isFalse.append(jit.branch32(CCallHelpers::NotEqual, T9, A4));
    jit.load32(Address(A2, StringImpl::flagsOffset()), T9);
    // Atoms are unique per content.
    isFalse.append(jit.branchTest32(CCallHelpers::NonZero, T9, TrustedImm32(StringImpl::flagIsAtom())));
    slow.append(jit.branchTest32(CCallHelpers::Zero, T9, TrustedImm32(StringImpl::flagIs8Bit())));
    jit.loadPtr(Address(A2, StringImpl::dataOffset()), A2);

    // Compare A4 characters at A2 with those at A3.
    CCallHelpers::Label compare = jit.label();
    jit.loadPtr(Address(A3, StringImpl::dataOffset()), A3);
    CCallHelpers::Label eightAtATime = jit.label();
    Jump fewerThanEight = jit.branch32(CCallHelpers::Below, A4, TrustedImm32(8));
    jit.load64(Address(A2), T9);
    jit.load64(Address(A3), T11);
    isFalse.append(jit.branch64(CCallHelpers::NotEqual, T9, T11));
    jit.add64(TrustedImm32(8), A2);
    jit.add64(TrustedImm32(8), A3);
    jit.sub32(TrustedImm32(8), A4);
    jit.jump().linkTo(eightAtATime, &jit);
    fewerThanEight.link(&jit);
    CCallHelpers::Label oneAtATime = jit.label();
    isTrue.append(jit.branchTest32(CCallHelpers::Zero, A4));
    jit.load8(Address(A2), T9);
    jit.load8(Address(A3), T11);
    isFalse.append(jit.branch32(CCallHelpers::NotEqual, T9, T11));
    jit.add64(TrustedImm32(1), A2);
    jit.add64(TrustedImm32(1), A3);
    jit.sub32(TrustedImm32(1), A4);
    jit.jump().linkTo(oneAtATime, &jit);

    isRope.link(&jit);
    jit.load32(Address(A0, JSRopeString::offsetOfLength()), T9);
    isFalse.append(jit.branch32(CCallHelpers::NotEqual, T9, A4));
    // A substring of an 8-bit string is compared in place, without resolving it. See JSRopeString::CompactFibers; the base string
    // is always resolved.
    constexpr uintptr_t narrowSlice = JSRopeString::isSubstringInPointer | JSRopeString::is8BitInPointer;
    jit.and64(TrustedImm32(narrowSlice), A2, T9);
    slow.append(jit.branch64(CCallHelpers::NotEqual, T9, TrustedImm32(narrowSlice)));
    jit.load64(Address(A0, JSRopeString::offsetOfFiber1()), T9);
    jit.load64(Address(A0, JSRopeString::offsetOfFiber2()), T11);
    jit.urshift64(TrustedImm32(32), T9);
    jit.and64(TrustedImm32(0xffff), T11, A2);
    jit.lshift64(TrustedImm32(32), A2);
    jit.or64(T9, A2);
    jit.urshift64(TrustedImm32(16), T11);
    jit.loadPtr(Address(A2, JSString::offsetOfValue()), A2);
    jit.loadPtr(Address(A2, StringImpl::dataOffset()), A2);
    jit.add64(T11, A2);
    jit.jump().linkTo(compare, &jit);

    isTrue.link(&jit);
    jit.move(TrustedImm32(1), A0);
    jit.ret();
    isFalse.link(&jit);
    jit.move(TrustedImm32(0), A0);
    jit.ret();

    // A rope, or a 16-bit string. A0 and A1 are unchanged.
    slow.link(&jit);
    generateEqual(jit, Entry::operationAOTCompareStrictEq);
}
static void generateLooseEqual(CCallHelpers& jit) { generateEqual(jit, Entry::operationAOTCompareEq); }

// Calls operation(globalObject, A0, A1) for the call site in T10.
static void callBinaryOperation(CCallHelpers& jit, Entry operation)
{
    loadInstance(jit, T11);
    jit.move(A1, A2);
    jit.move(A0, A1);
    jit.loadPtr(Address(T11, Instance::offsetOfGlobalObject()), A0);
    jit.loadPtr(Address(T11, Instance::offsetOfRuntimeTable()), T11);
    jit.loadPtr(Address(T11, static_cast<unsigned>(operation) * sizeof(void*)), T11);
    callAndCheckException(jit, T11, Returns::Value);
}

enum class Binary : uint8_t { Add, Sub, Mul, BitAnd, BitOr, BitXor, LShift, RShift, URShift, Mod, Less, LessEq, Greater, GreaterEq };

// A function with no loops may still be called from one, so the stub handles numbers itself. Everything else goes to the runtime.
static void generateBinary(CCallHelpers& jit, Binary kind, Entry operation)
{
    bool isComparison = kind >= Binary::Less;
    bool hasDoubleCase = isComparison || kind <= Binary::Mul;
    constexpr FPRReg left = FPRInfo::fpRegT0;
    constexpr FPRReg right = FPRInfo::fpRegT1;
    CCallHelpers::JumpList notBothInt32;
    CCallHelpers::JumpList slow;

    jit.move(CCallHelpers::TrustedImm64(JSValue::NumberTag), T9);
    notBothInt32.append(jit.branch64(CCallHelpers::Below, A0, T9));
    notBothInt32.append(jit.branch64(CCallHelpers::Below, A1, T9));
    auto boxInt32AndReturn = [&] {
        jit.zeroExtend32ToWord(T11, T11);
        jit.or64(T9, T11, A0);
        jit.ret();
    };
    auto compareAndReturn = [&](CCallHelpers::RelationalCondition condition) {
        jit.compare32(condition, A0, A1, A0);
        jit.ret();
    };
    switch (kind) {
    case Binary::Add:
        slow.append(jit.branchAdd32(CCallHelpers::Overflow, A0, A1, T11));
        boxInt32AndReturn();
        break;
    case Binary::Sub:
        slow.append(jit.branchSub32(CCallHelpers::Overflow, A0, A1, T11));
        boxInt32AndReturn();
        break;
    case Binary::Mul:
        slow.append(jit.branchMul32(CCallHelpers::Overflow, A0, A1, T11));
        slow.append(jit.branchTest32(CCallHelpers::Zero, T11)); // The result may be -0.
        boxInt32AndReturn();
        break;
    case Binary::BitAnd:
        jit.and32(A0, A1, T11);
        boxInt32AndReturn();
        break;
    case Binary::BitOr:
        jit.or32(A0, A1, T11);
        boxInt32AndReturn();
        break;
    case Binary::BitXor:
        jit.xor32(A0, A1, T11);
        boxInt32AndReturn();
        break;
    case Binary::LShift:
        jit.lshift32(A0, A1, T11);
        boxInt32AndReturn();
        break;
    case Binary::RShift:
        jit.rshift32(A0, A1, T11);
        boxInt32AndReturn();
        break;
    case Binary::URShift:
        jit.urshift32(A0, A1, T11);
        slow.append(jit.branch32(CCallHelpers::LessThan, T11, TrustedImm32(0))); // The result is nonnegative but does not fit in an int32.
        boxInt32AndReturn();
        break;
    case Binary::Mod:
        // Only a nonnegative dividend and a positive divisor. The other cases involve signed zeros and division by zero.
        slow.append(jit.branch32(CCallHelpers::LessThan, A0, TrustedImm32(0)));
        slow.append(jit.branch32(CCallHelpers::LessThanOrEqual, A1, TrustedImm32(0)));
        jit.div32(A0, A1, T11);
        jit.multiplySub32(T11, A1, A0, T11);
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

    notBothInt32.link(&jit);
    if (hasDoubleCase) {
        slow.append(jit.branchTest64(CCallHelpers::Zero, A0, T9));
        slow.append(jit.branchTest64(CCallHelpers::Zero, A1, T9));
        auto toDouble = [&](GPRReg value, FPRReg result) {
            Jump isInt32 = jit.branch64(CCallHelpers::AboveOrEqual, value, T9);
            jit.add64(T9, value, T11);
            jit.move64ToDouble(T11, result);
            Jump done = jit.jump();
            isInt32.link(&jit);
            jit.convertInt32ToDouble(value, result);
            done.link(&jit);
        };
        toDouble(A0, left);
        toDouble(A1, right);
        auto compareDoublesAndReturn = [&](CCallHelpers::DoubleCondition condition) {
            jit.compareDouble(condition, left, right, A0);
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
            jit.moveDoubleTo64(left, A0);
            jit.sub64(T9, A0);
            jit.ret();
        }
    }

    slow.link(&jit);
    callBinaryOperation(jit, operation);
}

static void generateAdd(CCallHelpers& jit) { generateBinary(jit, Binary::Add, Entry::operationAOTValueAdd); }
static void generateMod(CCallHelpers& jit) { generateBinary(jit, Binary::Mod, Entry::operationAOTValueMod); }
static void generateSub(CCallHelpers& jit) { generateBinary(jit, Binary::Sub, Entry::operationAOTValueSub); }
static void generateMul(CCallHelpers& jit) { generateBinary(jit, Binary::Mul, Entry::operationAOTValueMul); }
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

// A0 is a cell, and the int32 in A1 did not index into a butterfly. If A0 is a fixed-length typed array and the index is in bounds,
// leaves the array's JSType in T13, its storage pointer in T11 and the zero-extended index in T12.
static void checkTypedArrayAccess(CCallHelpers& jit, CCallHelpers::JumpList& slow)
{
    jit.load8(Address(A0, JSCell::typeInfoTypeOffset()), T13);
    jit.sub32(T13, TrustedImm32(FirstTypedArrayType), T11);
    slow.append(jit.branch32(CCallHelpers::AboveOrEqual, T11, TrustedImm32(NumberOfTypedArrayTypesExcludingDataView)));
    slow.append(jit.branchTest8(CCallHelpers::NonZero, Address(A0, JSArrayBufferView::offsetOfMode()), TrustedImm32(isResizableOrGrowableSharedMode)));
    jit.zeroExtend32ToWord(A1, T12);
    // A detached array has length zero.
    jit.loadPtr(Address(A0, JSArrayBufferView::offsetOfLength()), T11);
    slow.append(jit.branchPtr(CCallHelpers::AboveOrEqual, T12, T11));
    jit.loadPtr(Address(A0, JSArrayBufferView::offsetOfVector()), T11);
}

static void getFromMegamorphicCache(CCallHelpers&, GPRReg uid, CCallHelpers::JumpList& notFound);

// Execution falls through into the next stub. (The padding that the assembler inserts to align a stub is not executable: it traps.)
static void goOnIntoNextStub(CCallHelpers& jit)
{
    while (jit.m_assembler.codeSize() % 16)
        jit.nop();
}

// Boxes the integer in A1.
static void boxIntegerInA1(CCallHelpers& jit)
{
    jit.move(CCallHelpers::TrustedImm64(JSValue::NumberTag), T9);
    jit.signExtend32ToPtr(A1, T11);
    Jump isInt32 = jit.branch64(CCallHelpers::Equal, A1, T11);
    jit.convertInt64ToDouble(A1, FPRInfo::fpRegT0);
    jit.moveDoubleTo64(FPRInfo::fpRegT0, A1);
    jit.sub64(T9, A1);
    Jump done = jit.jump();
    isInt32.link(&jit);
    jit.zeroExtend32ToWord(A1, A1);
    jit.or64(T9, A1);
    done.link(&jit);
}

static void generateGetByValAtIndex(CCallHelpers& jit)
{
    static_assert(static_cast<unsigned>(Stub::GetByValAtIndex) + 1 == static_cast<unsigned>(Stub::GetByVal));
    // An in-bounds, non-hole element in storage that holds JSValues.
    CCallHelpers::JumpList otherwise;
    otherwise.append(jit.branchIfNotCell(A0));
    jit.load8(Address(A0, JSCell::indexingTypeAndMiscOffset()), T13);
    jit.and32(TrustedImm32(IndexingShapeMask), T13);
    Jump contiguousCase = jit.branch32(CCallHelpers::Equal, T13, TrustedImm32(ContiguousShape));
    otherwise.append(jit.branch32(CCallHelpers::NotEqual, T13, TrustedImm32(Int32Shape)));
    contiguousCase.link(&jit);
    jit.loadPtr(Address(A0, JSObject::butterflyOffset()), T11);
    jit.load32(Address(T11, Butterfly::offsetOfPublicLength()), T12);
    // A negative index compares as larger than any length.
    otherwise.append(jit.branch64(CCallHelpers::AboveOrEqual, A1, T12));
    jit.load64(CCallHelpers::BaseIndex(T11, A1, CCallHelpers::TimesEight), T11);
    otherwise.append(jit.branchTest64(CCallHelpers::Zero, T11));
    jit.move(T11, A0);
    jit.ret();

    otherwise.link(&jit);
    boxIntegerInA1(jit);
    goOnIntoNextStub(jit);
}

static void generateGetByVal(CCallHelpers& jit)
{
    constexpr FPRReg number = FPRInfo::fpRegT0;
    CCallHelpers::JumpList slow;
    jit.move(CCallHelpers::TrustedImm64(JSValue::NumberTag), T9);
    Jump notInt32 = jit.branch64(CCallHelpers::Below, A1, T9);
    CCallHelpers::Label haveIndex = jit.label();
    slow.append(jit.branchIfNotCell(A0));
    jit.load8(Address(A0, JSCell::indexingTypeAndMiscOffset()), T13);
    jit.and32(TrustedImm32(IndexingShapeMask), T13);
    Jump noButterfly = jit.branchTest32(CCallHelpers::Zero, T13);
    jit.loadPtr(Address(A0, JSObject::butterflyOffset()), T11);
    jit.zeroExtend32ToWord(A1, T12);
    Jump doubleCase = jit.branch32(CCallHelpers::Equal, T13, TrustedImm32(DoubleShape));
    slow.append(jit.branch32(CCallHelpers::Above, T13, TrustedImm32(ContiguousShape)));
    static_assert(Int32Shape < DoubleShape && DoubleShape < ContiguousShape);
    slow.append(jit.branch32(CCallHelpers::Below, T13, TrustedImm32(Int32Shape)));

    // An in-bounds, non-hole element in storage that holds JSValues.
    slow.append(jit.branch32(CCallHelpers::AboveOrEqual, T12, Address(T11, Butterfly::offsetOfPublicLength())));
    jit.load64(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesEight), T11);
    slow.append(jit.branchTest64(CCallHelpers::Zero, T11));
    jit.move(T11, A0);
    jit.ret();

    auto boxInt32AndReturn = [&] {
        jit.or64(T9, T11, A0);
        jit.ret();
    };
    auto boxDoubleAndReturn = [&] {
        jit.moveDoubleTo64(number, A0);
        jit.sub64(T9, A0);
        jit.ret();
    };

    // A hole is represented as NaN.
    doubleCase.link(&jit);
    slow.append(jit.branch32(CCallHelpers::AboveOrEqual, T12, Address(T11, Butterfly::offsetOfPublicLength())));
    jit.loadDouble(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesEight), number);
    slow.append(jit.branchIfNaN(number));
    boxDoubleAndReturn();

    noButterfly.link(&jit);
    // A character of a string, if it has a VM-wide single-character string.
    Jump notString = jit.branchIfNotString(A0);
    jit.loadPtr(Address(A0, JSString::offsetOfValue()), T11);
    slow.append(jit.branchIfRopeStringImpl(T11));
    jit.zeroExtend32ToWord(A1, T12);
    slow.append(jit.branch32(CCallHelpers::AboveOrEqual, T12, Address(T11, StringImpl::lengthMemoryOffset())));
    jit.load32(Address(T11, StringImpl::flagsOffset()), T13);
    jit.loadPtr(Address(T11, StringImpl::dataOffset()), T11);
    Jump is16Bit = jit.branchTest32(CCallHelpers::Zero, T13, TrustedImm32(StringImpl::flagIs8Bit()));
    jit.load8(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesOne), T11);
    Jump haveCharacter = jit.jump();
    is16Bit.link(&jit);
    jit.load16(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesTwo), T11);
    slow.append(jit.branch32(CCallHelpers::Above, T11, TrustedImm32(maxSingleCharacterString)));
    haveCharacter.link(&jit);
    loadInstance(jit, T12);
    jit.loadPtr(Address(T12, Instance::offsetOfVM()), T12);
    jit.addPtr(TrustedImm32(OBJECT_OFFSETOF(VM, smallStrings) + SmallStrings::offsetOfSingleCharacterStrings()), T12);
    jit.loadPtr(CCallHelpers::BaseIndex(T12, T11, CCallHelpers::TimesEight), A0);
    jit.ret();

    notString.link(&jit);
    checkTypedArrayAccess(jit, slow);
    auto ofType = [&](JSType type, const auto& load) {
        Jump other = jit.branch32(CCallHelpers::NotEqual, T13, TrustedImm32(type));
        load();
        other.link(&jit);
    };
    ofType(Int32ArrayType, [&] {
        jit.load32(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesFour), T11);
        boxInt32AndReturn();
    });
    ofType(Uint8ArrayType, [&] {
        jit.load8(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesOne), T11);
        boxInt32AndReturn();
    });
    ofType(Float64ArrayType, [&] {
        jit.loadDouble(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesEight), number);
        jit.purifyNaN(number, number);
        boxDoubleAndReturn();
    });
    ofType(Uint8ClampedArrayType, [&] {
        jit.load8(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesOne), T11);
        boxInt32AndReturn();
    });
    ofType(Uint16ArrayType, [&] {
        jit.load16(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesTwo), T11);
        boxInt32AndReturn();
    });
    ofType(Int16ArrayType, [&] {
        jit.load16SignedExtendTo32(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesTwo), T11);
        boxInt32AndReturn();
    });
    ofType(Int8ArrayType, [&] {
        jit.load8SignedExtendTo32(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesOne), T11);
        boxInt32AndReturn();
    });
    ofType(Uint32ArrayType, [&] {
        jit.load32(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesFour), T11);
        Jump isInt32 = jit.branch32(CCallHelpers::GreaterThanOrEqual, T11, TrustedImm32(0));
        jit.convertInt64ToDouble(T11, number);
        boxDoubleAndReturn();
        isInt32.link(&jit);
        boxInt32AndReturn();
    });
    ofType(Float32ArrayType, [&] {
        jit.loadFloat(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesFour), number);
        jit.convertFloatToDouble(number, number);
        jit.purifyNaN(number, number);
        boxDoubleAndReturn();
    });

    // A double with an integral value names the same property as the integer. So does -0.
    notInt32.link(&jit);
    Jump notNumber = jit.branchTest64(CCallHelpers::Zero, A1, T9);
    jit.add64(T9, A1, T11);
    jit.move64ToDouble(T11, number);
    jit.branchConvertDoubleToInt32(number, T11, slow, FPRInfo::fpRegT1, false);
    jit.or64(T9, T11, A1);
    jit.jump().linkTo(haveIndex, &jit);

    // A property name: an atom string or a symbol.
    notNumber.link(&jit);
    slow.append(jit.branchIfNotCell(A0));
    slow.append(jit.branchIfNotObject(A0));
    slow.append(jit.branchIfNotCell(A1));
    Jump isSymbol = jit.branchIfSymbol(A1);
    slow.append(jit.branchIfNotString(A1));
    jit.loadPtr(Address(A1, JSString::offsetOfValue()), A2);
    slow.append(jit.branchIfRopeStringImpl(A2));
    slow.append(jit.branchTest32(CCallHelpers::Zero, Address(A2, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIsAtom())));
    Jump haveName = jit.jump();
    isSymbol.link(&jit);
    jit.loadPtr(Address(A1, Symbol::offsetOfSymbolImpl()), A2);
    haveName.link(&jit);
    getFromMegamorphicCache(jit, A2, slow);

    slow.link(&jit);
    callBinaryOperation(jit, Entry::operationAOTGetByVal);
}

static void generatePutByValAtIndex(CCallHelpers& jit)
{
    static_assert(static_cast<unsigned>(Stub::PutByValAtIndex) + 1 == static_cast<unsigned>(Stub::PutByVal));
    boxIntegerInA1(jit);
    goOnIntoNextStub(jit);
}

static void generatePutByVal(CCallHelpers& jit)
{
    // An element within the capacity of contiguous storage that is not copy-on-write.
    CCallHelpers::JumpList slow;
    jit.move(CCallHelpers::TrustedImm64(JSValue::NumberTag), T9);
    slow.append(jit.branch64(CCallHelpers::Below, A1, T9));
    slow.append(jit.branchIfNotCell(A0));
    jit.load8(Address(A0, JSCell::indexingTypeAndMiscOffset()), T11);
    jit.and32(TrustedImm32(IndexingShapeMask | CopyOnWrite), T11);
    Jump isContiguous = jit.branch32(CCallHelpers::Equal, T11, TrustedImm32(ContiguousShape));
    slow.append(jit.branchTest32(CCallHelpers::NonZero, T11));

    // A typed array, and a number that only needs truncation.
    {
        constexpr FPRReg number = FPRInfo::fpRegT0;
        checkTypedArrayAccess(jit, slow);
        Jump valueIsNotInt32 = jit.branch64(CCallHelpers::Below, A2, T9);
        auto ofType = [&](JSType type, const auto& store) {
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
        ofType(Int32ArrayType, store32);
        ofType(Uint8ArrayType, store8);
        ofType(Uint32ArrayType, store32);
        ofType(Uint16ArrayType, store16);
        ofType(Int16ArrayType, store16);
        ofType(Int8ArrayType, store8);
        jit.convertInt32ToDouble(A2, number);
        ofType(Float64ArrayType, storeDouble);
        ofType(Float32ArrayType, storeFloat);
        slow.append(jit.jump());

        valueIsNotInt32.link(&jit);
        slow.append(jit.branchTest64(CCallHelpers::Zero, A2, T9));
        jit.add64(T9, A2, A4);
        jit.move64ToDouble(A4, number);
        ofType(Float64ArrayType, storeDouble);
        ofType(Float32ArrayType, storeFloat);
        slow.append(jit.jump());
    }

    isContiguous.link(&jit);
    jit.loadPtr(Address(A0, JSObject::butterflyOffset()), T11);
    jit.zeroExtend32ToWord(A1, T12);
    Jump inBounds = jit.branch32(CCallHelpers::Below, T12, Address(T11, Butterfly::offsetOfPublicLength()));
    slow.append(jit.branch32(CCallHelpers::AboveOrEqual, T12, Address(T11, Butterfly::offsetOfVectorLength())));
    // The slots between the old length and this index are already holes.
    jit.add32(TrustedImm32(1), T12, T13);
    jit.store32(T13, Address(T11, Butterfly::offsetOfPublicLength()));
    inBounds.link(&jit);
    jit.store64(A2, CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesEight));

    Jump notCell = jit.branchIfNotCell(A2);
    loadInstance(jit, T9);
    jit.load8(Address(A0, JSCell::cellStateOffset()), T11);
    jit.loadPtr(Address(T9, Instance::offsetOfVM()), T12);
    jit.load32(Address(T12, VM::offsetOfHeapBarrierThreshold()), T13);
    Jump barrier = jit.branch32(CCallHelpers::BelowOrEqual, T11, T13);
    notCell.link(&jit);
    jit.ret();

    barrier.link(&jit);
    jit.move(A0, A1);
    jit.move(T12, A0);
    jit.loadPtr(Address(T9, Instance::offsetOfRuntimeTable()), T9);
    jit.loadPtr(Address(T9, static_cast<unsigned>(Entry::operationAOTWriteBarrier) * sizeof(void*)), T9);
    jit.farJump(T9, OperationPtrTag);

    slow.link(&jit);
    loadInstance(jit, T11);
    jit.move(A3, A4);
    jit.move(A2, A3);
    jit.move(A1, A2);
    jit.move(A0, A1);
    jit.loadPtr(Address(T11, Instance::offsetOfGlobalObject()), A0);
    jit.loadPtr(Address(T11, Instance::offsetOfRuntimeTable()), T11);
    jit.loadPtr(Address(T11, static_cast<unsigned>(Entry::operationAOTPutByVal) * sizeof(void*)), T11);
    callAndCheckException(jit, T11, Returns::Void);
}

static void generatePutByValDirect(CCallHelpers& jit)
{
    // An element of an array with JSValue storage that is not copy-on-write, within its capacity. Usually an append.
    CCallHelpers::JumpList slow;
    jit.move(CCallHelpers::TrustedImm64(JSValue::NumberTag), T9);
    slow.append(jit.branch64(CCallHelpers::Below, A1, T9));
    jit.load8(Address(A0, JSCell::indexingTypeAndMiscOffset()), T11);
    jit.and32(TrustedImm32(IsArray | IndexingShapeMask | CopyOnWrite), T11);
    slow.append(jit.branch32(CCallHelpers::NotEqual, T11, TrustedImm32(ArrayWithContiguous)));
    jit.loadPtr(Address(A0, JSObject::butterflyOffset()), T11);
    jit.zeroExtend32ToWord(A1, T12);
    slow.append(jit.branch32(CCallHelpers::AboveOrEqual, T12, Address(T11, Butterfly::offsetOfVectorLength())));
    jit.store64(A2, CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesEight));
    Jump isWithinLength = jit.branch32(CCallHelpers::Below, T12, Address(T11, Butterfly::offsetOfPublicLength()));
    jit.add32(TrustedImm32(1), T12, T13);
    jit.store32(T13, Address(T11, Butterfly::offsetOfPublicLength()));
    isWithinLength.link(&jit);

    Jump notCell = jit.branchIfNotCell(A2);
    loadInstance(jit, T9);
    jit.load8(Address(A0, JSCell::cellStateOffset()), T11);
    jit.loadPtr(Address(T9, Instance::offsetOfVM()), T12);
    jit.load32(Address(T12, VM::offsetOfHeapBarrierThreshold()), T13);
    Jump barrier = jit.branch32(CCallHelpers::BelowOrEqual, T11, T13);
    notCell.link(&jit);
    jit.ret();

    barrier.link(&jit);
    jit.move(A0, A1);
    jit.move(T12, A0);
    jit.loadPtr(Address(T9, Instance::offsetOfRuntimeTable()), T9);
    jit.loadPtr(Address(T9, static_cast<unsigned>(Entry::operationAOTWriteBarrier) * sizeof(void*)), T9);
    jit.farJump(T9, OperationPtrTag);

    slow.link(&jit);
    loadInstance(jit, T11);
    jit.move(A3, A4);
    jit.move(A2, A3);
    jit.move(A1, A2);
    jit.move(A0, A1);
    jit.loadPtr(Address(T11, Instance::offsetOfGlobalObject()), A0);
    jit.loadPtr(Address(T11, Instance::offsetOfRuntimeTable()), T11);
    jit.loadPtr(Address(T11, static_cast<unsigned>(Entry::operationAOTPutByValDirect) * sizeof(void*)), T11);
    callAndCheckException(jit, T11, Returns::Void);
}

// ---- Sites

// A site is passed as the address of its Slot. The function has its Data at hand, so this puts the first thing a stub reads one
// load away. The site's index only matters on a miss.
static Address slotWord(GPRReg slot, unsigned word) { return Address(slot, word * sizeof(void*)); }

// data: the Data. slot: the address of one of its slots. Leaves the slot's index in `result`.
static void siteOfSlot(CCallHelpers& jit, GPRReg data, GPRReg slot, GPRReg result)
{
    static_assert(sizeof(Slot) == 16);
    jit.subPtr(slot, data, result);
    jit.subPtr(TrustedImm32(Data::offsetOfSlots()), result);
    jit.urshiftPtr(TrustedImm32(4), result);
}

// Cache miss. The operands are in A0..., followed by the site. Calls
//     operation(globalObject, operands..., identifier, Slot*, extra)
// This part places the arguments and leaves the operation's address in T9.
static void prepareMissAtSite(CCallHelpers& jit, Entry operation, unsigned numberOfOperands)
{
    RELEASE_ASSERT(numberOfOperands >= 1 && numberOfOperands <= 3);
    constexpr GPRReg arguments[] = { A0, A1, A2, A3, A4, A5, GPRInfo::argumentGPR6 };
    GPRReg site = arguments[numberOfOperands];
    loadIndexOfCaller(jit);
    loadInstanceAndDataOfSlot(jit, site, T9, T10);
    siteOfSlot(jit, T10, site, T13);
    loadInfo(jit, T9, T11);
    jit.loadPtr(Address(T11, FunctionInfo::offsetOfSites()), T11);
    static_assert(sizeof(Site) == 4);
    jit.load32(CCallHelpers::BaseIndex(T11, T13, CCallHelpers::TimesFour), T12);
    jit.move(site, T11);

    for (unsigned i = numberOfOperands; i--;)
        jit.move(arguments[i], arguments[i + 1]);
    GPRReg identifier = arguments[numberOfOperands + 1];
    GPRReg slot = arguments[numberOfOperands + 2];
    GPRReg extra = arguments[numberOfOperands + 3];
    jit.and32(TrustedImm32((1u << Site::identifierBits) - 1), T12, identifier);
    jit.urshift32(T12, TrustedImm32(Site::identifierBits), extra);
    jit.move(T11, slot);
    jit.loadPtr(Address(T9, Instance::offsetOfGlobalObject()), A0);
    jit.loadPtr(Address(T9, Instance::offsetOfRuntimeTable()), T9);
    jit.loadPtr(Address(T9, static_cast<unsigned>(operation) * sizeof(void*)), T9);
}

static void missAtSite(CCallHelpers& jit, Entry operation, unsigned numberOfOperands, Returns returns)
{
    prepareMissAtSite(jit, operation, numberOfOperands);
    callAndCheckException(jit, T9, returns);
}

// word: the first word of a slot whose offset is a locationOfProperty(). object: the object that holds the property. Leaves the
// storage base in `storage` and the index into it in `word`.
static void locateCachedProperty(CCallHelpers& jit, GPRReg object, GPRReg word, GPRReg storage)
{
    jit.lshift64(word, TrustedImm32(32 - Slot::offsetBits), word);
    jit.rshift64(word, TrustedImm32(64 - Slot::offsetBits), word);
    jit.loadPtr(Address(object, JSObject::butterflyOffset()), storage);
    jit.moveConditionally64(CCallHelpers::LessThan, word, TrustedImm32(0), storage, object, storage);
}

static Address slotOfFrameBeingMade(CallFrameSlot slot, ptrdiff_t offset = 0)
{
    return Address(CCallHelpers::stackPointerRegister, (static_cast<int>(slot) - CallerFrameAndPC::sizeInRegisters) * static_cast<int>(sizeof(Register)) + offset);
}

// A0 = a cell. slot: the slot of a property access. If the cell has a known shape (Structure::knownShape()), looks up the shape and
// the site's selector in the program's dispatch table and leaves the property's location in T12: in words from the start of the
// object or, if negative, from its butterfly. Expects T9 = the Instance, T10 = the Data that contains the slot and T15 = the
// function index, and preserves them. Leaves the selector in the given register. Clobbers T11 and T13.
static void findInDispatchTable(CCallHelpers& jit, GPRReg slot, GPRReg selector, CCallHelpers::JumpList& notOfKnownShape, CCallHelpers::JumpList& notOwn)
{
    jit.load32(Address(A0, JSCell::structureIDOffset()), T11);
    jit.or64(CCallHelpers::TrustedImm64(structureIDBaseOfImages), T11);
    jit.load16(Address(T11, Structure::offsetOfKnownShape()), T11);
    notOfKnownShape.append(jit.branchTest32(CCallHelpers::Zero, T11));
    loadInfo(jit, T9, T12);
    jit.load16(Address(T12, FunctionInfo::offsetOfFlags()), selector);
    siteOfSlot(jit, T10, slot, T13);
    static_assert(sizeof(Site) == 4);
    Jump isWhatTheSiteReads = jit.branchTest32(CCallHelpers::NonZero, selector, TrustedImm32(FunctionInfo::sitesHaveTheirConstants));
    notOfKnownShape.append(jit.branchTest32(CCallHelpers::Zero, selector, TrustedImm32(FunctionInfo::hasSiteConstants)));
    jit.loadPtr(Address(T12, FunctionInfo::offsetOfSites()), T12);
    jit.load32(Address(T12, static_cast<ptrdiff_t>(OBJECT_OFFSETOF(ImageFunction, numSlots)) - static_cast<ptrdiff_t>(sizeof(ImageFunction))), selector);
    jit.getEffectiveAddress(CCallHelpers::BaseIndex(T12, selector, CCallHelpers::TimesFour), T12);
    jit.load32(CCallHelpers::BaseIndex(T12, T13, CCallHelpers::TimesFour), selector);
    Jump haveSelector = jit.jump();
    isWhatTheSiteReads.link(&jit);
    jit.loadPtr(Address(T12, FunctionInfo::offsetOfSites()), T12);
    jit.load32(CCallHelpers::BaseIndex(T12, T13, CCallHelpers::TimesFour), selector);
    jit.and32(TrustedImm32((1u << Site::identifierBits) - 1), selector);
    haveSelector.link(&jit);
    notOfKnownShape.append(jit.branchTest32(CCallHelpers::Zero, selector));
    jit.loadPtr(Address(T9, Instance::offsetOfRowsOfSelectors()), T12);
    jit.load32(CCallHelpers::BaseIndex(T12, selector, CCallHelpers::TimesFour), T12);
    jit.add32(T11, T12);
    jit.loadPtr(Address(T9, Instance::offsetOfDispatch()), T13);
    jit.load32(CCallHelpers::BaseIndex(T13, T12, CCallHelpers::TimesFour), T12);
    jit.urshift32(T12, TrustedImm32(ImageDispatchEntry::locationBits), T13);
    notOwn.append(jit.branch32(CCallHelpers::NotEqual, T13, selector));
    jit.lshift64(TrustedImm32(64 - ImageDispatchEntry::locationBits), T12);
    jit.rshift64(TrustedImm32(64 - ImageDispatchEntry::locationBits), T12);
}

// Call after the above, for an inline property. If the site's slot is empty, fills it in, because inline fast paths read the slot
// without calling a stub. Preserves A0-A2, T9, T10 and T12.
static void fillEmptySlotFromDispatchTable(CCallHelpers& jit, GPRReg slot)
{
    constexpr GPRReg word = A4;
    constexpr GPRReg scratch = A5;
    jit.loadPtr(Address(T9, Instance::offsetOfSharedData()), scratch);
    Jump slotIsNobodys = jit.branchPtr(CCallHelpers::Equal, T10, scratch);
    jit.load64(slotWord(slot, 0), word);
    Jump slotIsTaken = jit.branchTest32(CCallHelpers::NonZero, word);
    Jump slotHasMore = jit.branchTestPtr(CCallHelpers::NonZero, slotWord(slot, 1));
    // Tell the GC that this function's slots have been filled.
    Jump collectorKnows = jit.branchTest8(CCallHelpers::NonZero, Address(T10, Data::offsetOfHasBeenFilledSinceLastCollection()));
    jit.subPtr(TrustedImm32(80), CCallHelpers::stackPointerRegister);
    jit.storePair64(A0, A1, CCallHelpers::stackPointerRegister, TrustedImm32(0));
    jit.storePair64(A2, A3, CCallHelpers::stackPointerRegister, TrustedImm32(16));
    jit.storePair64(T9, T10, CCallHelpers::stackPointerRegister, TrustedImm32(32));
    jit.storePair64(T12, word, CCallHelpers::stackPointerRegister, TrustedImm32(48));
    jit.storePtr(CCallHelpers::linkRegister, Address(CCallHelpers::stackPointerRegister, 64));
    jit.loadPtr(Address(T9, Instance::offsetOfRuntimeTable()), T9);
    jit.loadPtr(Address(T9, static_cast<unsigned>(Entry::operationAOTNoteFilled) * sizeof(void*)), T9);
    jit.move(T10, A0);
    jit.call(T9, OperationPtrTag);
    jit.loadPair64(CCallHelpers::stackPointerRegister, TrustedImm32(0), A0, A1);
    jit.loadPair64(CCallHelpers::stackPointerRegister, TrustedImm32(16), A2, A3);
    jit.loadPair64(CCallHelpers::stackPointerRegister, TrustedImm32(32), T9, T10);
    jit.loadPair64(CCallHelpers::stackPointerRegister, TrustedImm32(48), T12, word);
    jit.loadPtr(Address(CCallHelpers::stackPointerRegister, 64), CCallHelpers::linkRegister);
    jit.addPtr(TrustedImm32(80), CCallHelpers::stackPointerRegister);
    collectorKnows.link(&jit);
    jit.and64(CCallHelpers::TrustedImm64(static_cast<int64_t>(static_cast<uint64_t>(Slot::attemptsMask) << 32)), word);
    jit.lshift64(T12, TrustedImm32(32), scratch);
    jit.or64(scratch, word);
    jit.load32(Address(A0, JSCell::structureIDOffset()), scratch);
    jit.or64(scratch, word);
    jit.store64(word, slotWord(slot, 0));
    jit.load64(Address(T10, Data::offsetOfSlotEpoch()), word);
    jit.add64(TrustedImm32(1), word);
    jit.store64(word, Address(T10, Data::offsetOfSlotEpoch()));
    slotIsNobodys.link(&jit);
    slotIsTaken.link(&jit);
    slotHasMore.link(&jit);
}

// A0 = the base, T12 = a GetterSetter. If its getter is a function, tail-calls it, so that it returns to the stub's caller.
// Otherwise preserves A0 and A1.
static void callGetter(CCallHelpers& jit, CCallHelpers::JumpList& cannot)
{
    jit.loadPtr(Address(T12, GetterSetter::offsetOfGetter()), T12);
    {
        // The length of a typed array, unless it must be computed (resizable buffer) or does not fit in an int32.
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfGetterOfLengthOfTypedArrays()), T13);
        Jump isSomeOtherGetter = jit.branchPtr(CCallHelpers::NotEqual, T12, T13);
        jit.load8(Address(A0, JSCell::typeInfoTypeOffset()), T13);
        jit.sub32(TrustedImm32(FirstTypedArrayType), T13);
        Jump isNoTypedArray = jit.branch32(CCallHelpers::AboveOrEqual, T13, TrustedImm32(NumberOfTypedArrayTypesExcludingDataView));
        jit.load8(Address(A0, JSArrayBufferView::offsetOfMode()), T13);
        Jump takesWorkingOut = jit.branchTest32(CCallHelpers::NonZero, T13, TrustedImm32(resizabilityAndAutoLengthMask));
        jit.load64(Address(A0, JSArrayBufferView::offsetOfLength()), T13);
        Jump isTooLong = jit.branch64(CCallHelpers::Above, T13, CCallHelpers::TrustedImm64(std::numeric_limits<int32_t>::max()));
        jit.move(CCallHelpers::TrustedImm64(JSValue::NumberTag), T11);
        jit.or64(T11, T13, A0);
        jit.ret();
        isSomeOtherGetter.link(&jit);
        isNoTypedArray.link(&jit);
        takesWorkingOut.link(&jit);
        isTooLong.link(&jit);
    }
    cannot.append(jit.branchIfNotType(T12, JSFunctionType));
    jit.move(A0, thisGPR);
    jit.move(T12, calleeGPR);
    jit.move(TrustedImm32(0), countGPR);
    jit.jump().linkTo(s_labels[static_cast<unsigned>(Stub::Call)], &jit);
}

// A0 = the base, an object. uid: the property name (an atom, or a symbol's uid). On a hit in the VM's megamorphic cache, returns
// the value from the stub, or calls the getter if it is one (see callGetter()). On a miss, preserves A0, A1, T10 and uid.
static void getFromMegamorphicCache(CCallHelpers& jit, GPRReg uid, CCallHelpers::JumpList& notFound)
{
    constexpr GPRReg cache = GPRInfo::argumentGPR7;
    constexpr GPRReg result = GPRInfo::argumentGPR6;
    ASSERT(uid != cache && uid != result && uid != A0 && uid != A1);
    loadInstance(jit, cache);
    jit.loadPtr(Address(cache, Instance::offsetOfRuntimeTable()), cache);
    jit.loadPtr(Address(cache, static_cast<unsigned>(Entry::MegamorphicCache) * sizeof(void*)), cache);
    notFound.append(jit.branchTestPtr(CCallHelpers::Zero, cache));
    CCallHelpers::JumpList notValue = jit.loadMegamorphicProperty(CCallHelpers::MegamorphicCacheLocation(cache), A0, uid, nullptr, result, T11, T12, T13);
    jit.move(result, A0);
    jit.ret();
    notValue.link(&jit);
    notFound.append(jit.loadMegamorphicGetterSetter(CCallHelpers::MegamorphicCacheLocation(cache), A0, uid, nullptr, result, T11, T12, T13));
    jit.move(result, T12);
    callGetter(jit, notFound);
}

static void generateGetByIdWith(CCallHelpers&, Entry);
// Continuations for when the property is not inline at the location the slot gives. The base is in A0 and the slot in A1. For the
// first two, the slot's first word is in T11.
struct WaysOnOfGetById {
    CCallHelpers::Label isIndirect;
    CCallHelpers::Label isOfAnotherStructure;
    CCallHelpers::Label miss;
};
static WaysOnOfGetById s_waysOnOfGetById[2];
static WaysOnOfGetById& waysOnOfGetById(Entry operation) { return s_waysOnOfGetById[operation == Entry::operationAOTGetByIdWellKnown]; }

// For acceptsOperandInAnyRegister(): the start of generateGetByIdWith() for a base in another register.
static void generateGetByIdFrom(CCallHelpers& jit, Entry operation, GPRReg base)
{
    ASSERT(base != A0 && base != A1 && base != T11 && base != T12);
    Jump isNotCell = jit.branchIfNotCell(base);
    jit.load64(slotWord(A1, 0), T11);
    jit.load32(Address(base, JSCell::structureIDOffset()), T12);
    Jump isOfAnotherStructure = jit.branch32(CCallHelpers::NotEqual, T11, T12);
    Jump isIndirect = jit.branchTest64(CCallHelpers::NonZero, T11, CCallHelpers::TrustedImm64(static_cast<int64_t>(Slot::isIndirect) << 32));
    jit.extractUnsignedBitfield64(T11, TrustedImm32(32), TrustedImm32(Slot::directLocationBits), T11);
    jit.load64(CCallHelpers::BaseIndex(base, T11, CCallHelpers::TimesEight), A0);
    jit.ret();

    auto& waysOn = waysOnOfGetById(operation);
    isIndirect.link(&jit);
    jit.move(base, A0);
    jit.jump().linkTo(waysOn.isIndirect, &jit);
    isOfAnotherStructure.link(&jit);
    jit.move(base, A0);
    jit.jump().linkTo(waysOn.isOfAnotherStructure, &jit);
    isNotCell.link(&jit);
    jit.move(base, A0);
    jit.jump().linkTo(waysOn.miss, &jit);
}

// The fast path depends neither on the object's Structure nor on the call site, only on whether the Structure's field ID for the
// slot matches.
// Continuations for a mismatched field ID (the ID is in T11 and the Structure in T13) and for a base that cannot be checked. The
// base is in A0.
struct WaysOnOfReadSlot {
    CCallHelpers::Label isNotThere;
    CCallHelpers::Label miss;
};
static WaysOnOfReadSlot s_waysOnOfReadSlot[2][Structure::numberOfSlotsWithFieldIDs];

// base: see acceptsOperandInAnyRegister().
static void generateReadSlot(CCallHelpers& jit, unsigned slot, bool allowsUndefined, GPRReg base = A0)
{
    static_assert(Structure::numberOfSlotsWithFieldIDs == 8);
    ASSERT(base != A1 && base != T11 && base != T12 && base != T13);
    Jump isNotCell = jit.branchIfNotCell(base);
    jit.load32(Address(base, JSCell::structureIDOffset()), T13);
#if CPU(ARM64)
    static_assert(!(structureIDBaseOfImages & ~(0xffffull << 32)));
    jit.m_assembler.movk<64>(T13, static_cast<uint16_t>(structureIDBaseOfImages >> 32), 32);
#else
    jit.or64(CCallHelpers::TrustedImm64(structureIDBaseOfImages), T13);
#endif
    jit.load16(Address(T13, Structure::offsetOfFieldIDInSlot() + slot * sizeof(uint16_t)), T11);
    Jump isNotThere = jit.branch32(CCallHelpers::NotEqual, T11, A1);
    jit.load64(Address(base, JSObject::offsetOfInlineStorage() + slot * sizeof(EncodedJSValue)), A0);
    jit.ret();

    auto& waysOn = s_waysOnOfReadSlot[allowsUndefined][slot];
    if (base != A0) {
        isNotThere.link(&jit);
        jit.move(base, A0);
        jit.jump().linkTo(waysOn.isNotThere, &jit);
        isNotCell.link(&jit);
        jit.move(base, A0);
        jit.jump().linkTo(waysOn.miss, &jit);
        return;
    }

    isNotThere.link(&jit);
    waysOn.isNotThere = jit.label();
    CCallHelpers::JumpList miss;
    if (allowsUndefined) {
        // None of the layout's fields occupies the slot. If the object's layout is the field's layout, the object has no such
        // property, and does not inherit one either (see Instance::convertToTypedLayout()).
        miss.append(jit.branchTest32(CCallHelpers::NonZero, T11));
        jit.load16(Address(T13, Structure::offsetOfTypedLayoutID()), T11);
        loadInstance(jit, T12);
        jit.loadPtr(Address(T12, Instance::offsetOfRuntimeTable()), T12);
        jit.loadPtr(Address(T12, (static_cast<unsigned>(Entry::LayoutIDsOfFieldsInSlot0) + slot) * sizeof(void*)), T12);
        jit.load16(CCallHelpers::BaseIndex(T12, A1, CCallHelpers::TimesTwo), T12);
        miss.append(jit.branch32(CCallHelpers::NotEqual, T11, T12));
        jit.move(CCallHelpers::TrustedImm64(JSValue::encode(jsUndefined())), A0);
        jit.ret();
    }
    miss.link(&jit);
    isNotCell.link(&jit);
    waysOn.miss = jit.label();
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
#undef AOT_READ_SLOT

static void generateGetById(CCallHelpers& jit) { generateGetByIdWith(jit, Entry::operationAOTGetById); }
static void generateGetByIdWellKnown(CCallHelpers& jit) { generateGetByIdWith(jit, Entry::operationAOTGetByIdWellKnown); }

static void generateGetByIdWith(CCallHelpers& jit, Entry operation)
{
    CCallHelpers::JumpList miss;
    // Callers keep the tag registers valid.
    miss.append(jit.branchIfNotCell(A0));
    jit.load64(slotWord(A1, 0), T11);
    jit.load32(Address(A0, JSCell::structureIDOffset()), T12);
    Jump isOfAnotherStructure = jit.branch32(CCallHelpers::NotEqual, T11, T12);
    // Inline storage, the most common case.
    Jump isIndirect = jit.branchTest64(CCallHelpers::NonZero, T11, CCallHelpers::TrustedImm64(static_cast<int64_t>(Slot::isIndirect) << 32));
#if CPU(ARM64)
    jit.extractUnsignedBitfield64(T11, TrustedImm32(32), TrustedImm32(Slot::directLocationBits), T11);
#else
    jit.urshift64(TrustedImm32(32), T11);
    jit.and32(TrustedImm32(Slot::directLocationMask), T11);
#endif
    jit.load64(CCallHelpers::BaseIndex(A0, T11, CCallHelpers::TimesEight), A0);
    jit.ret();

    isIndirect.link(&jit);
    waysOnOfGetById(operation).isIndirect = jit.label();
    // Out-of-line storage, or a holder on the prototype chain shared by every base with this Structure.
    jit.loadPtr(slotWord(A1, 1), T12);
    jit.moveConditionallyTest64(CCallHelpers::NonZero, T12, T12, T12, A0, T12);
    Jump isGetter = jit.branchTest64(CCallHelpers::NonZero, T11, CCallHelpers::TrustedImm64(static_cast<int64_t>(Slot::isGetter) << 32));
    locateCachedProperty(jit, T12, T11, T13);
    jit.load64(CCallHelpers::BaseIndex(T13, T11, CCallHelpers::TimesEight), A0);
    jit.ret();

    isGetter.link(&jit);
    locateCachedProperty(jit, T12, T11, T13);
    jit.load64(CCallHelpers::BaseIndex(T13, T11, CCallHelpers::TimesEight), T12);
    callGetter(jit, miss);

    isOfAnotherStructure.link(&jit);
    waysOnOfGetById(operation).isOfAnotherStructure = jit.label();
    CCallHelpers::JumpList missAndUpdateCache;
    if (operation == Entry::operationAOTGetById) {
        constexpr GPRReg several = T13;
        constexpr GPRReg ownSlotOfSite = T14;
        {
            // A direct slot with a property name id: the base's Structure may have the same name at the same inline offset (a base-class field in another subclass, say).
            // T11 = the slot's first word, T12 = the base's StructureID.
            Jump isIndirect = jit.branchTest64(CCallHelpers::NonZero, T11, CCallHelpers::TrustedImm64(static_cast<int64_t>(Slot::isIndirect) << 32));
            jit.extractUnsignedBitfield64(T11, TrustedImm32(32 + Slot::nameIDShift), TrustedImm32(16), T13);
            Jump hasNoNameID = jit.branchTest32(CCallHelpers::Zero, T13);
            jit.extractUnsignedBitfield64(T11, TrustedImm32(32), TrustedImm32(Slot::directLocationBits), T14);
            jit.or64(CCallHelpers::TrustedImm64(structureIDBaseOfImages), T12);
            constexpr ptrdiff_t wordsBeforeInlineStorage = JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue);
            jit.load16(CCallHelpers::BaseIndex(T12, T14, CCallHelpers::TimesTwo, Structure::offsetOfFieldIDInSlot() - wordsBeforeInlineStorage * sizeof(uint16_t)), T12);
            Jump isAnotherName = jit.branch32(CCallHelpers::NotEqual, T12, T13);
            jit.load64(CCallHelpers::BaseIndex(A0, T14, CCallHelpers::TimesEight), A0);
            jit.ret();
            isIndirect.link(&jit);
            hasNoNameID.link(&jit);
            isAnotherName.link(&jit);
        }
        // The slot holds a different Structure: this is the second one the site has seen.
        missAndUpdateCache.append(jit.branchTest32(CCallHelpers::NonZero, T11));
        // The slot has no Structure. It is either empty or polymorphic (Slot::isPolymorphic()).
        jit.urshift64(T11, TrustedImm32(32), T12);
        jit.and32(TrustedImm32(Slot::flagsMask), T12);
        miss.append(jit.branch32(CCallHelpers::NotEqual, T12, TrustedImm32(Slot::polymorphicFlags)));
        jit.loadPtr(slotWord(A1, 1), several);
        jit.load32(Address(A0, JSCell::structureIDOffset()), T12);
        static_assert(!OBJECT_OFFSETOF(Slot, structureID));
        Jump isThatOne[PolymorphicSlots::numberOfSlots];
        for (unsigned i = 0; i < PolymorphicSlots::numberOfSlots; ++i)
            isThatOne[i] = jit.branch32(CCallHelpers::Equal, T12, Address(several, PolymorphicSlots::offsetOfSlots() + i * sizeof(Slot)));

        // No entry matches.
        missAndUpdateCache.append(jit.branchTest32(CCallHelpers::NonZero, Address(several, PolymorphicSlots::offsetOfTimesLeftToLearnAtOnce())));
        jit.load32(Address(several, PolymorphicSlots::offsetOfMisses()), T12);
        jit.add32(TrustedImm32(1), T12);
        jit.store32(T12, Address(several, PolymorphicSlots::offsetOfMisses()));
        missAndUpdateCache.append(jit.branchTest32(CCallHelpers::Zero, T12, TrustedImm32(PolymorphicSlots::missesBetweenLearning - 1)));
        missAndUpdateCache.append(jit.branchIfNotObject(A0));
        jit.loadPtr(Address(several, PolymorphicSlots::offsetOfName()), A2);
        getFromMegamorphicCache(jit, A2, missAndUpdateCache);

        // An entry matches. From here on it is treated as the site's slot.
        CCallHelpers::JumpList haveSlot;
        for (unsigned i = 0; i < PolymorphicSlots::numberOfSlots; ++i) {
            isThatOne[i].link(&jit);
            jit.move(A1, ownSlotOfSite);
            jit.addPtr(TrustedImm32(PolymorphicSlots::offsetOfSlots() + i * sizeof(Slot)), several, A1);
            if (i + 1 < PolymorphicSlots::numberOfSlots)
                haveSlot.append(jit.jump());
        }
        haveSlot.link(&jit);
        jit.load64(slotWord(A1, 0), T11);
        Jump isIntricateThere = jit.branchTest64(CCallHelpers::NonZero, T11, CCallHelpers::TrustedImm64(static_cast<int64_t>(Slot::isIndirect) << 32));
        jit.extractUnsignedBitfield64(T11, TrustedImm32(32), TrustedImm32(Slot::directLocationBits), T11);
        jit.load64(CCallHelpers::BaseIndex(A0, T11, CCallHelpers::TimesEight), A0);
        jit.ret();
        isIntricateThere.link(&jit);
        jit.loadPtr(slotWord(A1, 1), T12);
        jit.moveConditionallyTest64(CCallHelpers::NonZero, T12, T12, T12, A0, T12);
        Jump isGetterThere = jit.branchTest64(CCallHelpers::NonZero, T11, CCallHelpers::TrustedImm64(static_cast<int64_t>(Slot::isGetter) << 32));
        locateCachedProperty(jit, T12, T11, T13);
        jit.load64(CCallHelpers::BaseIndex(T13, T11, CCallHelpers::TimesEight), A0);
        jit.ret();
        isGetterThere.link(&jit);
        locateCachedProperty(jit, T12, T11, T13);
        jit.load64(CCallHelpers::BaseIndex(T13, T11, CCallHelpers::TimesEight), T12);
        CCallHelpers::JumpList cannotCallGetter;
        callGetter(jit, cannotCallGetter);
        cannotCallGetter.link(&jit);
        jit.move(ownSlotOfSite, A1);
        missAndUpdateCache.append(jit.jump());
    }

    miss.link(&jit);
    waysOnOfGetById(operation).miss = jit.label();
    if (operation == Entry::operationAOTGetById) {
        loadIndexOfCaller(jit);
        // An object with a known shape: the program's dispatch table gives the locations of its properties.
        CCallHelpers::JumpList notInTable;
        CCallHelpers::JumpList notOwn;
        loadInstanceAndDataOfSlot(jit, A1, T9, T10);
        countMissOfSlot(jit, T9, T10);
        notInTable.append(jit.branchIfNotCell(A0));
        findInDispatchTable(jit, A1, A2, notInTable, notOwn);
        Jump isOutOfLine = jit.branch64(CCallHelpers::LessThan, T12, TrustedImm32(0));
        fillEmptySlotFromDispatchTable(jit, A1);
        jit.load64(CCallHelpers::BaseIndex(A0, T12, CCallHelpers::TimesEight), A0);
        jit.ret();

        isOutOfLine.link(&jit);
        jit.loadPtr(Address(A0, JSObject::butterflyOffset()), T13);
        jit.load64(CCallHelpers::BaseIndex(T13, T12, CCallHelpers::TimesEight), A0);
        jit.ret();

        // No own property with that name. See Instance::lookAtObjectPrototype().
        notOwn.link(&jit);
        jit.load32(Address(A0, JSCell::structureIDOffset()), T12);
        jit.or64(CCallHelpers::TrustedImm64(structureIDBaseOfImages), T12);
        jit.loadPtr(Address(T12, Structure::prototypeOffset()), T12);
        jit.loadPtr(Address(T9, Instance::offsetOfObjectPrototype()), T13);
        notInTable.append(jit.branchPtr(CCallHelpers::NotEqual, T12, T13));
        jit.load32(Address(T13, JSCell::structureIDOffset()), T12);
        notInTable.append(jit.branch32(CCallHelpers::NotEqual, T12, Address(T9, Instance::offsetOfStructureIDOfObjectPrototype())));
        jit.loadPtr(Address(T9, Instance::offsetOfSelectorsOnObjectPrototype()), T12);
        jit.urshift32(A2, TrustedImm32(3), T13);
        jit.load8(CCallHelpers::BaseIndex(T12, T13, CCallHelpers::TimesOne), T12);
        jit.and32(TrustedImm32(7), A2, T13);
        jit.urshift32(T13, T12);
        notInTable.append(jit.branchTest32(CCallHelpers::NonZero, T12, TrustedImm32(1)));
        jit.move(CCallHelpers::TrustedImm64(JSValue::ValueUndefined), A0);
        jit.ret();
        notInTable.link(&jit);

        // See branchIfSlotIsStillOfUse() (AOTThunks.cpp).
        CCallHelpers::JumpList notFound;
        notFound.append(jit.branchIfNotCell(A0));
        notFound.append(jit.branchIfNotObject(A0));
        jit.load32(Address(A1, OBJECT_OFFSETOF(Slot, offset)), T11);
        jit.and32(TrustedImm32(Slot::attemptsMask), T11);
        Jump hasGivenUp = jit.branch32(CCallHelpers::Equal, T11, TrustedImm32(Slot::attemptsMask));
        loadInstanceAndDataOfSlot(jit, A1, T9, T10);
        Jump isTaken = jit.branchTest32(CCallHelpers::NonZero, Address(A1, OBJECT_OFFSETOF(Slot, structureID)));
        jit.loadPtr(Address(T9, Instance::offsetOfSharedData()), T11);
        notFound.append(jit.branchPtr(CCallHelpers::NotEqual, T10, T11));
        Jump isNobodys = jit.jump();
        hasGivenUp.link(&jit);
        loadInstanceAndDataOfSlot(jit, A1, T9, T10);
        isTaken.link(&jit);
        isNobodys.link(&jit);
        siteOfSlot(jit, T10, A1, T13);
        loadInfo(jit, T9, T10);
        jit.loadPtr(Address(T10, FunctionInfo::offsetOfSites()), T11);
        jit.load32(CCallHelpers::BaseIndex(T11, T13, CCallHelpers::TimesFour), T12);
        jit.and32(TrustedImm32((1u << Site::identifierBits) - 1), T12);
        jit.loadPtr(Address(T10, FunctionInfo::offsetOfIdentifiers()), T11);
        jit.loadPtr(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesEight), A2);
        getFromMegamorphicCache(jit, A2, notFound);
        notFound.link(&jit);
    }
    missAtSite(jit, operation, 1, Returns::Value);
    if (!missAndUpdateCache.empty()) {
        // Bypass the front end that probes the megamorphic cache first (generateFrontEndGetById()): a hit there would keep the
        // site's own cache from ever being filled.
        missAndUpdateCache.link(&jit);
        missAtSite(jit, Entry::RawGetById, 1, Returns::Value);
    }
}

static void generatePutById(CCallHelpers& jit)
{
    CCallHelpers::JumpList miss;
    miss.append(jit.branchIfNotCell(A0));
    jit.load64(slotWord(A2, 0), T11);
    jit.load32(Address(A0, JSCell::structureIDOffset()), T12);
    miss.append(jit.branch32(CCallHelpers::NotEqual, T11, T12));
    jit.load64(slotWord(A2, 1), T13);
    jit.urshift64(T13, TrustedImm32(32), T14);
    Jump slotHasFieldType = jit.branchTest32(CCallHelpers::NonZero, T14);
    CCallHelpers::Label isHeld = jit.label();
    locateCachedProperty(jit, A0, T11, T12);
    jit.store64(A1, CCallHelpers::BaseIndex(T12, T11, CCallHelpers::TimesEight));
    Jump sameStructure = jit.branchTest32(CCallHelpers::Zero, T13);
    jit.store32(T13, Address(A0, JSCell::structureIDOffset()));
    sameStructure.link(&jit);

    CCallHelpers::Label stored = jit.label();
    Jump notCell = jit.branchIfNotCell(A1);
    CCallHelpers::Label storedAndMayBeOfAnotherStructure = jit.label();
    loadInstance(jit, T9);
    jit.load8(Address(A0, JSCell::cellStateOffset()), T11);
    jit.loadPtr(Address(T9, Instance::offsetOfVM()), T12);
    jit.load32(Address(T12, VM::offsetOfHeapBarrierThreshold()), T13);
    Jump barrier = jit.branch32(CCallHelpers::BelowOrEqual, T11, T13);
    notCell.link(&jit);
    jit.ret();

    barrier.link(&jit);
    jit.move(A0, A1);
    jit.move(T12, A0);
    jit.loadPtr(Address(T9, Instance::offsetOfRuntimeTable()), T9);
    jit.loadPtr(Address(T9, static_cast<unsigned>(Entry::operationAOTWriteBarrier) * sizeof(void*)), T9);
    jit.farJump(T9, OperationPtrTag);

    // A typed field (Slot::fieldType). T14 = the accepted kinds, with the typed layout ID for objects above them. A value that
    // obviously qualifies is stored. Anything that needs a closer look goes to the runtime.
    {
        slotHasFieldType.link(&jit);
        auto ifAccepts = [&](unsigned kind) {
            miss.append(jit.branchTest32(CCallHelpers::Zero, T14, TrustedImm32(kind)));
            jit.jump().linkTo(isHeld, &jit);
        };
        Jump isCell = jit.branchIfCell(A1);
        Jump isNotNumber = jit.branchIfNotNumber(A1);
        miss.append(jit.branchTest32(CCallHelpers::Zero, T14, TrustedImm32(SoundTypeNumber)));
        jit.branchIfNotInt32(A1).linkTo(isHeld, &jit);
        // Typed fields store every number as a double.
        jit.convertInt32ToDouble(A1, FPRInfo::fpRegT0);
        jit.moveDoubleTo64(FPRInfo::fpRegT0, A1);
        jit.move(CCallHelpers::TrustedImm64(JSValue::NumberTag), T12);
        jit.sub64(T12, A1);
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
            // If the field's strings are atomized, a string that is not obviously an atom goes to the runtime to be atomized.
            Jump anyStringWillDo = jit.branchTest32(CCallHelpers::Zero, T14, TrustedImm32(TypedLayoutTable::stringsAreAtoms));
            jit.load8(Address(A1, JSCell::typeInfoFlagsOffset()), T12);
            Jump isAtom = jit.branchTest32(CCallHelpers::NonZero, T12, TrustedImm32(TypeInfoPerCellBit));
            // A long string need not be an atom and is stored as it is.
            jit.loadPtr(Address(A1, JSString::offsetOfValue()), T12);
            Jump isInPieces = jit.branchIfRopeStringImpl(T12);
            jit.load32(Address(T12, StringImpl::lengthMemoryOffset()), T12);
            Jump haveLength = jit.jump();
            isInPieces.link(&jit);
            jit.load32(Address(A1, JSRopeString::offsetOfLength()), T12);
            haveLength.link(&jit);
            miss.append(jit.branch32(CCallHelpers::BelowOrEqual, T12, TrustedImm32(TypedLayoutTable::maxLengthOfAtomizedString)));
            isAtom.link(&jit);
            anyStringWillDo.link(&jit);
        }
        ifAccepts(SoundTypeString);
        isNotString.link(&jit);
        Jump isNotArray = jit.branch32(CCallHelpers::NotEqual, T12, TrustedImm32(ArrayType));
        ifAccepts(SoundTypeArray);
        isNotArray.link(&jit);
        miss.append(jit.branch32(CCallHelpers::NotEqual, T12, TrustedImm32(FinalObjectType)));
        miss.append(jit.branchTest32(CCallHelpers::Zero, T14, TrustedImm32(SoundTypeOtherObject)));
        jit.urshift32(T14, TrustedImm32(16), T12);
        jit.branchTest32(CCallHelpers::Zero, T12).linkTo(isHeld, &jit);
        loadInstance(jit, T9);
        jit.load32(Address(A1, JSCell::structureIDOffset()), T10);
        jit.or64(CCallHelpers::TrustedImm64(structureIDBaseOfImages), T10);
        jit.load16(Address(T10, Structure::offsetOfTypedLayoutID()), T10);
        jit.branch32(CCallHelpers::Equal, T10, T12).linkTo(isHeld, &jit);
        miss.append(jit.jump());
    }

    // A property of an object with a known shape. All such properties are plain writable data properties.
    miss.link(&jit);
    CCallHelpers::JumpList notInTable;
    loadIndexOfCaller(jit);
    loadInstanceAndDataOfSlot(jit, A2, T9, T10);
    countMissOfSlot(jit, T9, T10);
    // Not with typed fields: the dispatch table does not record which slots are typed. See TypedLayoutTable.
    if (!Options::useAOTTypedFields()) {
        notInTable.append(jit.branchIfNotCell(A0));
        findInDispatchTable(jit, A2, A3, notInTable, notInTable);
        Jump isOutOfLine = jit.branch64(CCallHelpers::LessThan, T12, TrustedImm32(0));
        fillEmptySlotFromDispatchTable(jit, A2);
        jit.store64(A1, CCallHelpers::BaseIndex(A0, T12, CCallHelpers::TimesEight));
        jit.jump().linkTo(stored, &jit);
        isOutOfLine.link(&jit);
        jit.loadPtr(Address(A0, JSObject::butterflyOffset()), T13);
        jit.store64(A1, CCallHelpers::BaseIndex(T13, T12, CCallHelpers::TimesEight));
        jit.jump().linkTo(stored, &jit);
    }

    notInTable.link(&jit);
    {
        // A typed field is never cached there. See PutPropertySlot::isCacheablePut().
        CCallHelpers::JumpList notFound;
        constexpr GPRReg cache = GPRInfo::argumentGPR7;
        notFound.append(jit.branchIfNotCell(A0));
        notFound.append(jit.branchIfNotObject(A0));
        loadInstanceAndDataOfSlot(jit, A2, T9, T10);
        siteOfSlot(jit, T10, A2, T13);
        loadInfo(jit, T9, T10);
        jit.loadPtr(Address(T10, FunctionInfo::offsetOfSites()), T11);
        jit.load32(CCallHelpers::BaseIndex(T11, T13, CCallHelpers::TimesFour), T12);
        // A store that defines the property regardless of the prototype chain is not what the cache holds.
        notFound.append(jit.branchTest32(CCallHelpers::NonZero, T12, TrustedImm32(1u << Site::identifierBits)));
        jit.and32(TrustedImm32((1u << Site::identifierBits) - 1), T12);
        jit.loadPtr(Address(T10, FunctionInfo::offsetOfIdentifiers()), T11);
        jit.loadPtr(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesEight), A3);
        jit.loadPtr(Address(T9, Instance::offsetOfRuntimeTable()), cache);
        jit.loadPtr(Address(cache, static_cast<unsigned>(Entry::MegamorphicCache) * sizeof(void*)), cache);
        notFound.append(jit.branchTestPtr(CCallHelpers::Zero, cache));
        auto [slow, reallocating] = jit.storeMegamorphicProperty(CCallHelpers::MegamorphicCacheLocation(cache), A0, A3, nullptr, A1, T11, T12, T13);
        jit.jump().linkTo(storedAndMayBeOfAnotherStructure, &jit);
        slow.link(&jit);
        reallocating.link(&jit);
        notFound.link(&jit);
    }
    missAtSite(jit, Entry::operationAOTPutById, 2, Returns::Void);
}

// Private name caches are keyed by Structure and name. The name is a cell that the slot points to.
static void checkPrivateNameCache(CCallHelpers& jit, GPRReg slot, CCallHelpers::JumpList& miss)
{
    miss.append(jit.branchIfNotCell(A0));
    jit.load64(slotWord(slot, 0), T11);
    jit.load32(Address(A0, JSCell::structureIDOffset()), T12);
    miss.append(jit.branch32(CCallHelpers::NotEqual, T11, T12));
    jit.loadPtr(slotWord(slot, 1), T12);
    miss.append(jit.branchPtr(CCallHelpers::NotEqual, T12, A1));
}

static void generateGetPrivateName(CCallHelpers& jit)
{
    CCallHelpers::JumpList miss;
    checkPrivateNameCache(jit, A2, miss);
    locateCachedProperty(jit, A0, T11, T12);
    jit.load64(CCallHelpers::BaseIndex(T12, T11, CCallHelpers::TimesEight), A0);
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
    // Only for an existing field.
    CCallHelpers::JumpList miss;
    checkPrivateNameCache(jit, A3, miss);
    locateCachedProperty(jit, A0, T11, T12);
    jit.store64(A2, CCallHelpers::BaseIndex(T12, T11, CCallHelpers::TimesEight));

    Jump notCell = jit.branchIfNotCell(A2);
    loadInstance(jit, T9);
    jit.load8(Address(A0, JSCell::cellStateOffset()), T11);
    jit.loadPtr(Address(T9, Instance::offsetOfVM()), T12);
    jit.load32(Address(T12, VM::offsetOfHeapBarrierThreshold()), T13);
    Jump barrier = jit.branch32(CCallHelpers::BelowOrEqual, T11, T13);
    notCell.link(&jit);
    jit.ret();

    barrier.link(&jit);
    jit.move(A0, A1);
    jit.move(T12, A0);
    jit.loadPtr(Address(T9, Instance::offsetOfRuntimeTable()), T9);
    jit.loadPtr(Address(T9, static_cast<unsigned>(Entry::operationAOTWriteBarrier) * sizeof(void*)), T9);
    jit.farJump(T9, OperationPtrTag);

    miss.link(&jit);
    missAtSite(jit, Entry::operationAOTPutPrivateName, 3, Returns::Void);
}

// T11 = the `offset` word of an op_resolve_scope's slot. A0 = the scope to start from. If the slot caches a depth, leaves the scope
// at that depth in A0. Otherwise takes the returned jump.
static Jump walkOutIfResolvedByDepth(CCallHelpers& jit)
{
    Jump miss = jit.branchTest32(CCallHelpers::Zero, T11, TrustedImm32(Slot::resolvesByDepth));
    jit.and32(TrustedImm32(~Slot::resolvesByDepth), T11);
    Jump there = jit.branchTest32(CCallHelpers::Zero, T11);
    auto loop = jit.label();
    jit.loadPtr(Address(A0, JSScope::offsetOfNext()), A0);
    jit.branchSub32(CCallHelpers::NonZero, TrustedImm32(1), T11).linkTo(loop, &jit);
    there.link(&jit);
    return miss;
}

static void generateResolveScope(CCallHelpers& jit)
{
    // See operationAOTResolveScope().
    loadInstance(jit, T9);
    jit.load32(slotWord(A1, 0).withOffset(sizeof(uint32_t)), T11);
    jit.loadPtr(Address(T9, Instance::offsetOfGlobalObject()), T12);
    jit.load32(Address(T12, JSGlobalObject::offsetOfGlobalLexicalBindingEpoch()), T12);
    jit.add32(TrustedImm32(1), T12);
    Jump notThatOne = jit.branch32(CCallHelpers::NotEqual, T11, T12);
    jit.loadPtr(slotWord(A1, 1), A0);
    jit.ret();

    notThatOne.link(&jit);
    Jump miss = walkOutIfResolvedByDepth(jit);
    jit.ret();

    miss.link(&jit);
    missAtSite(jit, Entry::operationAOTResolveScope, 1, Returns::Value);
}

// See operationAOTGetFromScope(). A0 = the scope. The slot occupies words firstWord and firstWord + 1 at A1. On a hit, returns from
// the stub with the value.
static void getFromScopeIfCached(CCallHelpers& jit, unsigned firstWord, CCallHelpers::JumpList& miss)
{
    jit.load64(slotWord(A1, firstWord), T11);
    jit.load32(Address(A0, JSCell::structureIDOffset()), T12);
    miss.append(jit.branch32(CCallHelpers::NotEqual, T11, T12));
    jit.loadPtr(slotWord(A1, firstWord + 1), T12);
    Jump noAddress = jit.branchTestPtr(CCallHelpers::Zero, T12);
    static_assert(Slot::pointerIsCell == 1u << 31);
    Jump isAddress = jit.branch64(CCallHelpers::GreaterThanOrEqual, T11, TrustedImm32(0));
    // See cacheVariableOfEnvironment().
    jit.loadPtr(Address(A0, JSSymbolTableObject::offsetOfSymbolTable()), T13);
    miss.append(jit.branchPtr(CCallHelpers::NotEqual, T12, T13));
    jit.urshift64(T11, TrustedImm32(32), T11);
    jit.and32(TrustedImm32(Slot::offsetMask), T11);
    jit.load64(CCallHelpers::BaseIndex(A0, T11, CCallHelpers::TimesEight, JSLexicalEnvironment::offsetOfVariables()), T12);
    miss.append(jit.branchTest64(CCallHelpers::Zero, T12));
    jit.move(T12, A0);
    jit.ret();
    isAddress.link(&jit);
    jit.load64(Address(T12), T12);
    miss.append(jit.branchTest64(CCallHelpers::Zero, T12));
    jit.move(T12, A0);
    jit.ret();

    noAddress.link(&jit);
    jit.urshift64(T11, TrustedImm32(32), T11);
    Jump outOfLine = jit.branch32(CCallHelpers::GreaterThanOrEqual, T11, TrustedImm32(firstOutOfLineOffset));
    jit.load64(CCallHelpers::BaseIndex(A0, T11, CCallHelpers::TimesEight, JSObject::offsetOfInlineStorage()), A0);
    jit.ret();
    outOfLine.link(&jit);
    jit.loadPtr(Address(A0, JSObject::butterflyOffset()), T12);
    jit.neg64(T11);
    jit.load64(CCallHelpers::BaseIndex(T12, T11, CCallHelpers::TimesEight, (firstOutOfLineOffset - 2) * static_cast<int>(sizeof(EncodedJSValue))), A0);
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
    // See cacheVariableOfEnvironment(), the only case that is cached.
    CCallHelpers::JumpList miss;
    jit.load64(slotWord(A2, 0), T11);
    jit.load32(Address(A0, JSCell::structureIDOffset()), T12);
    miss.append(jit.branch32(CCallHelpers::NotEqual, T11, T12));
    jit.loadPtr(slotWord(A2, 1), T12);
    jit.loadPtr(Address(A0, JSSymbolTableObject::offsetOfSymbolTable()), T13);
    miss.append(jit.branchPtr(CCallHelpers::NotEqual, T12, T13));
    jit.urshift64(T11, TrustedImm32(32), T11);
    jit.and32(TrustedImm32(Slot::offsetMask), T11);
    jit.store64(A1, CCallHelpers::BaseIndex(A0, T11, CCallHelpers::TimesEight, JSLexicalEnvironment::offsetOfVariables()));

    Jump notCell = jit.branchIfNotCell(A1);
    loadInstance(jit, T9);
    jit.load8(Address(A0, JSCell::cellStateOffset()), T11);
    jit.loadPtr(Address(T9, Instance::offsetOfVM()), T12);
    jit.load32(Address(T12, VM::offsetOfHeapBarrierThreshold()), T13);
    Jump barrier = jit.branch32(CCallHelpers::BelowOrEqual, T11, T13);
    notCell.link(&jit);
    jit.ret();

    barrier.link(&jit);
    jit.move(A0, A1);
    jit.move(T12, A0);
    jit.loadPtr(Address(T9, Instance::offsetOfRuntimeTable()), T9);
    jit.loadPtr(Address(T9, static_cast<unsigned>(Entry::operationAOTWriteBarrier) * sizeof(void*)), T9);
    jit.farJump(T9, OperationPtrTag);

    miss.link(&jit);
    missAtSite(jit, Entry::operationAOTPutToScope, 2, Returns::Void);
}

static void generateGetGlobal(CCallHelpers& jit)
{
    static_assert(sizeof(Slot) == 2 * sizeof(void*));
    loadInstance(jit, T9);
    jit.load32(slotWord(A1, 0).withOffset(sizeof(uint32_t)), T11);
    jit.loadPtr(Address(T9, Instance::offsetOfGlobalObject()), T12);
    jit.load32(Address(T12, JSGlobalObject::offsetOfGlobalLexicalBindingEpoch()), T12);
    jit.add32(TrustedImm32(1), T12);
    Jump notThatOne = jit.branch32(CCallHelpers::NotEqual, T11, T12);
    jit.loadPtr(slotWord(A1, 1), A0);

    auto resolved = jit.label();
    CCallHelpers::JumpList miss;
    getFromScopeIfCached(jit, 2, miss);

    miss.link(&jit);
    jit.addPtr(TrustedImm32(sizeof(Slot)), A1);
    missAtSite(jit, Entry::operationAOTGetFromScope, 1, Returns::Value);

    notThatOne.link(&jit);
    Jump notResolved = walkOutIfResolvedByDepth(jit);
    jit.jump().linkTo(resolved, &jit);

    notResolved.link(&jit);
    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(16), CCallHelpers::stackPointerRegister);
    jit.storePtr(A1, Address(CCallHelpers::stackPointerRegister));
    prepareMissAtSite(jit, Entry::operationAOTResolveScope, 1);
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
    jit.move(CCallHelpers::TrustedImm64(JSValue::NumberTag), T9);
    generic.append(jit.branchIfNotCell(A0));

    // An array with a butterfly stores its length there. A length that does not fit in an int32 goes to the runtime.
    jit.load8(Address(A0, JSCell::indexingTypeAndMiscOffset()), T11);
    Jump notArray = jit.branchTest32(CCallHelpers::Zero, T11, TrustedImm32(IsArray));
    Jump noStorage = jit.branchTest32(CCallHelpers::Zero, T11, TrustedImm32(IndexingShapeMask));
    jit.loadPtr(Address(A0, JSObject::butterflyOffset()), T11);
    jit.load32(Address(T11, Butterfly::offsetOfPublicLength()), T11);
    generic.append(jit.branch32(CCallHelpers::LessThan, T11, TrustedImm32(0)));
    jit.or64(T9, T11, A0);
    jit.ret();

    notArray.link(&jit);
    noStorage.link(&jit);
    generic.append(jit.branchIfNotType(A0, StringType));
    jit.loadPtr(Address(A0, JSString::offsetOfValue()), T11);
    Jump isRope = jit.branchIfRopeStringImpl(T11);
    jit.load32(Address(T11, StringImpl::lengthMemoryOffset()), T11);
    jit.or64(T9, T11, A0);
    jit.ret();
    isRope.link(&jit);
    jit.load32(Address(A0, JSRopeString::offsetOfLength()), T11);
    jit.or64(T9, T11, A0);
    jit.ret();

    generic.link(&jit);
    generateGetByIdWith(jit, Entry::operationAOTGetByIdWellKnown);
}

// ---- Exceptions

// vm: the VM. lookUp: operationLookupExceptionHandler(). Both are in caller-saved registers. The frame pointer and link register
// identify the location where the exception was noticed. Does not return.
static void unwind(CCallHelpers& jit, GPRReg vm, GPRReg lookUp)
{
    constexpr GPRReg keptVM = GPRInfo::regCS0;
    RELEASE_ASSERT(noOverlap(vm, lookUp, A0, GPRInfo::regT1));
    jit.emitFunctionPrologue();
    jit.move(vm, GPRInfo::regT1);
    jit.copyCalleeSavesToVMEntryFrameCalleeSavesBuffer(GPRInfo::regT1);
    // This frees them for use here: the handler restores them from there.
    jit.move(vm, keptVM);
    jit.move(vm, A0);
    jit.call(lookUp, OperationPtrTag);
    // genericUnwind() leaves the handler's frame in VM::callFrameForCatch and the target address in VM::targetMachinePCForThrow.
    // Whatever code is at the target, from any tier, starts by finding the VM from the frame it is entered in, which is still the
    // frame where the exception was noticed. A frame of AOT code has nothing to find it by, so it is made to look like a
    // WebAssembly frame: the CodeBlock slot holds an instance that contains the VM.
    constexpr ptrdiff_t offsetOfInstance = 4 * sizeof(Register);
    static_assert(static_cast<int>(CallFrameSlot::callee) < 4);
    jit.subPtr(TrustedImm32(WTF::roundUpToMultipleOf<stackAlignmentBytes()>(offsetOfInstance + Instance::offsetOfVM() + sizeof(void*))), CCallHelpers::stackPointerRegister);
    jit.move(CCallHelpers::stackPointerRegister, GPRInfo::callFrameRegister);
    jit.addPtr(TrustedImm32(offsetOfInstance), GPRInfo::callFrameRegister, GPRInfo::regT1);
    jit.storePtr(GPRInfo::regT1, CCallHelpers::addressFor(CallFrameSlot::codeBlock));
    jit.storePtr(keptVM, Address(GPRInfo::regT1, Instance::offsetOfVM()));
    jit.move(CCallHelpers::TrustedImm64(JSValue::NativeCalleeTag), GPRInfo::regT1);
    jit.store64(GPRInfo::regT1, CCallHelpers::addressFor(CallFrameSlot::callee));
    jit.loadPtr(Address(keptVM, OBJECT_OFFSETOF(VM, targetMachinePCForThrow)), GPRInfo::regT1);
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

// Runs in the frame of the function that ran out of stack, which is where the error is thrown.
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
    // The function has not saved any registers yet and its frame is not valid. The error is attributed to the caller.
    jit.emitFunctionEpilogue();
    throwStackOverflow(jit);
}

static void generateCatch(CCallHelpers& jit)
{
    // The frame pointer still belongs to the code that threw or noticed the exception. Like LLInt's handlers, this finds the VM
    // through that frame's callee.
    constexpr GPRReg vm = GPRInfo::regT3;
    jit.load64(CCallHelpers::addressFor(CallFrameSlot::callee), vm);
#if ENABLE(WEBASSEMBLY)
    jit.and64(CCallHelpers::TrustedImm64(JSValue::NativeCalleeMask), vm, GPRInfo::regT0);
    Jump isJSCallee = jit.branch64(CCallHelpers::NotEqual, GPRInfo::regT0, CCallHelpers::TrustedImm64(JSValue::NativeCalleeTag));
    jit.loadPtr(CCallHelpers::addressFor(CallFrameSlot::codeBlock), vm);
    jit.loadPtr(Address(vm, JSWebAssemblyInstance::offsetOfVM()), vm);
    Jump haveVM = jit.jump();
    isJSCallee.link(&jit);
#endif
    Jump isPreciseAllocation = jit.branchTestPtr(CCallHelpers::NonZero, vm, TrustedImm32(PreciseAllocation::halfAlignment));
    jit.andPtr(CCallHelpers::TrustedImmPtr(MarkedBlock::blockMask), vm);
    jit.loadPtr(Address(vm, MarkedBlock::offsetOfHeader + MarkedBlock::Header::offsetOfVM()), vm);
    Jump haveVMToo = jit.jump();
    isPreciseAllocation.link(&jit);
    jit.loadPtr(Address(vm, PreciseAllocation::offsetOfWeakSet() + WeakSet::offsetOfVM() - PreciseAllocation::headerSize()), vm);
    haveVMToo.link(&jit);
#if ENABLE(WEBASSEMBLY)
    haveVM.link(&jit);
#endif

    jit.restoreCalleeSavesFromVMEntryFrameCalleeSavesBuffer(vm, GPRInfo::regT0);
    jit.loadPtr(Address(vm, VM::callFrameForCatchOffset()), GPRInfo::callFrameRegister);
    jit.storePtr(CCallHelpers::TrustedImmPtr(nullptr), Address(vm, VM::callFrameForCatchOffset()));
    jit.loadPtr(Address(vm, OBJECT_OFFSETOF(VM, targetMachinePCAfterCatch)), GPRInfo::regT1);
    jit.farJump(GPRInfo::regT1, ExceptionHandlerPtrTag);
}

// ---- Entry from the standard calling convention

// The entry point for callers that use the engine's standard calling convention: the interpreter, C++, or a stub making a generic
// call. A standard frame is being set up and nothing has been pushed yet. It becomes the adapter's frame, and the adapter bridges
// the two conventions for as long as the function runs. word: the function's EntryWord.
static void adapt(CCallHelpers& jit, GPRReg word, GPRReg instance)
{
    ASSERT(noOverlap(word, instance, thisGPR, countGPR, calleeGPR, T13));
    jit.emitFunctionPrologue();
    static_assert(!(adapterSaveAreaSize % stackAlignmentBytes()));
    jit.subPtr(TrustedImm32(adapterSaveAreaSize), CCallHelpers::stackPointerRegister);
    jit.storePtr(instanceGPR, Address(GPRInfo::callFrameRegister, offsetOfInstanceRegisterInAdapter));
    jit.storePtr(GPRInfo::numberTagRegister, Address(GPRInfo::callFrameRegister, offsetOfNumberTagRegisterInAdapter));
    jit.storePtr(GPRInfo::notCellMaskRegister, Address(GPRInfo::callFrameRegister, offsetOfNotCellMaskRegisterInAdapter));
    jit.storePtr(instance, Address(GPRInfo::callFrameRegister, offsetOfInstanceInAdapter));
    jit.move(instance, instanceGPR);
    jit.emitMaterializeTagCheckRegisters();
    // Make sure that nothing mistakes it for an interpreter frame.
    jit.storePtr(CCallHelpers::TrustedImmPtr(nullptr), CCallHelpers::addressFor(CallFrameSlot::codeBlock));

    jit.load64(CCallHelpers::addressFor(CallFrameSlot::callee), calleeGPR);
    jit.load64(CCallHelpers::addressFor(CallFrameSlot::thisArgument), thisGPR);
    jit.load32(CCallHelpers::lowWordFor(CallFrameSlot::argumentCountIncludingThis), countGPR);
    jit.sub32(TrustedImm32(1), countGPR);
    Jump takesList = jit.branchTest64(CCallHelpers::NonZero, word, CCallHelpers::TrustedImm64(1LL << EntryWord::bitOfIsList));
    // The word above the last argument belongs to another frame but is mapped, so it may be loaded as long as it is not used.
    jit.move(CCallHelpers::TrustedImm64(JSValue::ValueUndefined), T13);
    for (unsigned i = 0; i < numberOfArgumentGPRs; ++i) {
        jit.load64(CCallHelpers::addressFor(virtualRegisterForArgumentIncludingThis(i + 1)), argumentGPR(i));
        jit.moveConditionally32(CCallHelpers::Above, countGPR, TrustedImm32(i), argumentGPR(i), T13, argumentGPR(i));
    }
    Jump ready = jit.jump();
    takesList.link(&jit);
    jit.move(countGPR, argumentGPR(0));
    jit.addPtr(TrustedImm32(virtualRegisterForArgumentIncludingThis(1).offset() * static_cast<int>(sizeof(Register))), GPRInfo::callFrameRegister, argumentGPR(1));
    ready.link(&jit);
    jit.and64(CCallHelpers::TrustedImm64(static_cast<int64_t>(EntryWord::addressMask)), word);
    jit.call(word, JSEntryPtrTag);
    s_returnsIntoAdapters->append(jit.label());

    jit.loadPtr(Address(GPRInfo::callFrameRegister, offsetOfInstanceRegisterInAdapter), instanceGPR);
    jit.loadPtr(Address(GPRInfo::callFrameRegister, offsetOfNumberTagRegisterInAdapter), GPRInfo::numberTagRegister);
    jit.loadPtr(Address(GPRInfo::callFrameRegister, offsetOfNotCellMaskRegisterInAdapter), GPRInfo::notCellMaskRegister);
    jit.emitFunctionEpilogue();
    jit.ret();
}

// Program or module code, which is entered with a CodeBlock in the frame, like an interpreter frame.
static void generateEnter(CCallHelpers& jit)
{
    jit.loadPtr(slotOfFrameBeingMade(CallFrameSlot::codeBlock), T11);
    jit.loadPtr(Address(T11, CodeBlock::offsetOfGlobalObject()), T12);
    jit.loadPtr(Address(T11, CodeBlock::jitCodeOffset()), T11);
    jit.loadPtr(Address(T12, JSGlobalObject::offsetOfAOTInstance()), T12);
    jit.loadPtr(Address(T11, JITCode::offsetOfEntry()), T11);
    adapt(jit, T11, T12);
}

// A function, which has no CodeBlock. Its callers leave the frame's CodeBlock slot empty.
static void generateEnterFunction(CCallHelpers& jit, CodeSpecializationKind kind)
{
    jit.loadPtr(slotOfFrameBeingMade(CallFrameSlot::callee), T11);
    jit.loadPtr(Address(T11, JSFunction::offsetOfExecutableOrRareData()), T11);
    Jump hasExecutable = jit.branchTestPtr(CCallHelpers::Zero, T11, TrustedImm32(JSFunction::rareDataTag));
    jit.loadPtr(Address(T11, FunctionRareData::offsetOfExecutable() - JSFunction::rareDataTag), T11);
    hasExecutable.link(&jit);
    jit.loadPtr(Address(T11, ExecutableBase::offsetOfJITCodeFor(kind)), T11);
    jit.loadPtr(Address(T11, JITCode::offsetOfInstance()), T12);
    jit.loadPtr(Address(T11, JITCode::offsetOfEntry()), T11);
    adapt(jit, T11, T12);
}
static void generateEnterFunctionForCall(CCallHelpers& jit) { generateEnterFunction(jit, CodeSpecializationKind::CodeForCall); }
static void generateEnterFunctionForConstruct(CCallHelpers& jit) { generateEnterFunction(jit, CodeSpecializationKind::CodeForConstruct); }

// ---- Calls

static void findTargetAndCall(CCallHelpers&);

// The frame is set up except for its CodeBlock. T0 = callee, T2 = the CallLinkInfo of a VirtualCallInfo, used only on the slow
// path.
template<typename LoadCallLinkInfo>
static void dispatchCall(CCallHelpers& jit, CodeSpecializationKind kind, const LoadCallLinkInfo& loadCallLinkInfo)
{
    auto slotOfNewFrame = [](CallFrameSlot slot, ptrdiff_t offset = 0) {
        return Address(CCallHelpers::stackPointerRegister, (static_cast<int>(slot) - CallerFrameAndPC::sizeInRegisters) * static_cast<int>(sizeof(Register)) + offset);
    };

    // A function that already has code, which covers nearly every callee.
    CCallHelpers::JumpList slow;
    slow.append(jit.branchIfNotCell(BaselineJITRegisters::Call::calleeGPR, DoNotHaveTagRegisters));
    slow.append(jit.branchIfNotFunction(BaselineJITRegisters::Call::calleeGPR));
    jit.loadPtr(Address(BaselineJITRegisters::Call::calleeGPR, JSFunction::offsetOfExecutableOrRareData()), T11);
    Jump hasExecutable = jit.branchTestPtr(CCallHelpers::Zero, T11, TrustedImm32(JSFunction::rareDataTag));
    jit.loadPtr(Address(T11, FunctionRareData::offsetOfExecutable() - JSFunction::rareDataTag), T11);
    hasExecutable.link(&jit);
    // A short-form executable does not store this. See ExecutableBase::wayIntoShortForm().
    Jump saysHowToGetIn = jit.branchIfNotType(T11, ShortFunctionExecutableType);
    jit.loadPtr(Address(T11, FunctionExecutable::offsetOfAOTEntryFor(kind)), T12);
    slow.append(jit.branchTestPtr(CCallHelpers::Zero, T12));
    if (kind == CodeSpecializationKind::CodeForConstruct) {
        jit.load32(Address(T11, FunctionExecutable::offsetOfAOTIndexFor(kind)), T11);
        Jump doesNotConstructByCalling = jit.branch32(CCallHelpers::NotEqual, T11, TrustedImm32(static_cast<int32_t>(FunctionExecutable::aotIndexOfWhatConstructsByCalling)));
        jit.farJump(T12, JSEntryPtrTag);
        doesNotConstructByCalling.link(&jit);
    }
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T12);
    jit.loadPtr(Address(T12, static_cast<unsigned>(isCall(kind) ? Entry::EnterStaticFunctionForCall : Entry::EnterStaticFunctionForConstruct) * sizeof(void*)), T12);
    jit.farJump(T12, JSEntryPtrTag);
    saysHowToGetIn.link(&jit);
    jit.loadPtr(Address(T11, ExecutableBase::offsetOfJITCodeWithArityCheckFor(kind)), T12);
    slow.append(jit.branchTestPtr(CCallHelpers::Zero, T12));
    Jump isNative = jit.branchIfNotType(T11, FunctionExecutableType);
    jit.loadPtr(Address(T11, FunctionExecutable::offsetOfCodeBlockFor(kind)), T11);
    jit.storePtr(T11, slotOfNewFrame(CallFrameSlot::codeBlock));
    isNative.link(&jit);
    jit.farJump(T12, JSEntryPtrTag);

    // Everything else is resolved by llint_virtual_call(), in the callee's frame. This may be a tail call, whose caller's frame is
    // gone, so everything needed comes with the CallLinkInfo.
    slow.link(&jit);
    loadCallLinkInfo();
    findTargetAndCall(jit);
}

static void findTargetAndCall(CCallHelpers& jit)
{
    constexpr GPRReg info = BaselineJITRegisters::Call::callLinkInfoGPR;
    jit.emitFunctionPrologue();
    // As in emitCallSlowPath(): the operation's frame reuses the space of the previous callee at this depth.
    jit.subPtr(TrustedImm32(stackBytesClearedForCallSlowPath), CCallHelpers::stackPointerRegister);
    static_assert(stackBytesClearedForCallSlowPath <= 256, "Or this wants to be a loop");
    for (size_t offset = 0; offset < stackBytesClearedForCallSlowPath; offset += 2 * sizeof(Register))
        jit.storePair64(ARM64Registers::zr, ARM64Registers::zr, CCallHelpers::stackPointerRegister, TrustedImm32(offset));
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
    // Returns the call target. A null target, returned together with the VM, means an exception was thrown.
    Jump threw = jit.branchTestPtr(CCallHelpers::NonZero, GPRInfo::returnValueGPR2);
    jit.farJump(GPRInfo::returnValueGPR, JSEntryPtrTag);

    threw.link(&jit);
    jit.move(GPRInfo::returnValueGPR2, T10);
    jit.loadPtr(Address(T11, VirtualCallInfo::offsetOfLookupExceptionHandler()), T11);
    unwind(jit, T10, T11);
}

// The entry point that a static FunctionExecutable advertises to callers using the standard calling convention. Such an executable
// is never written to. It records the function's index and the location of its code; everything else is found through the callee.
// On the first call the function is not yet linked to the realm, which findCallTarget() takes care of.
static void loadCalleeOfFrameBeingMadeAndItsVM(CCallHelpers& jit, GPRReg callee, GPRReg vm)
{
    jit.loadPtr(slotOfFrameBeingMade(CallFrameSlot::callee), callee);
    jit.move(callee, vm);
    Jump isPreciseAllocation = jit.branchTestPtr(CCallHelpers::NonZero, vm, TrustedImm32(PreciseAllocation::halfAlignment));
    jit.andPtr(CCallHelpers::TrustedImmPtr(MarkedBlock::blockMask), vm);
    jit.loadPtr(Address(vm, MarkedBlock::offsetOfHeader + MarkedBlock::Header::offsetOfVM()), vm);
    Jump haveVM = jit.jump();
    isPreciseAllocation.link(&jit);
    jit.loadPtr(Address(vm, PreciseAllocation::offsetOfWeakSet() + WeakSet::offsetOfVM() - PreciseAllocation::headerSize()), vm);
    haveVM.link(&jit);
}

static void generateEnterStaticFunction(CCallHelpers& jit, CodeSpecializationKind kind, Entry callLinkInfo)
{
    loadCalleeOfFrameBeingMadeAndItsVM(jit, T11, T12);
    jit.loadPtr(Address(T12, VM::offsetOfAOTInstanceOfProgram()), T12);

    jit.loadPtr(Address(T11, JSFunction::offsetOfExecutableOrRareData()), T11);
    Jump hasExecutable = jit.branchTestPtr(CCallHelpers::Zero, T11, TrustedImm32(JSFunction::rareDataTag));
    jit.loadPtr(Address(T11, FunctionRareData::offsetOfExecutable() - JSFunction::rareDataTag), T11);
    hasExecutable.link(&jit);
    jit.load32(Address(T11, FunctionExecutable::offsetOfAOTIndexFor(kind)), T13);
    jit.loadPtr(Address(T11, FunctionExecutable::offsetOfAOTEntryFor(kind)), T11);
    jit.addPtr(TrustedImm32(Instance::offsetOfStates()), T12, T9);
    jit.load32(CCallHelpers::BaseIndex(T9, T13, CCallHelpers::TimesFour), T9);
    Jump hasNothingYet = jit.branch32(CCallHelpers::Below, T9, TrustedImm32(Instance::isLinkedWithoutData));
    adapt(jit, T11, T12);

    hasNothingYet.link(&jit);
    jit.loadPtr(Address(T12, Instance::offsetOfRuntimeTable()), T11);
    jit.loadPtr(Address(T11, static_cast<unsigned>(callLinkInfo) * sizeof(void*)), BaselineJITRegisters::Call::callLinkInfoGPR);
    findTargetAndCall(jit);
}
static void generateEnterStaticFunctionForCall(CCallHelpers& jit) { generateEnterStaticFunction(jit, CodeSpecializationKind::CodeForCall, Entry::CallLinkInfoForCall); }
static void generateEnterStaticFunctionForConstruct(CCallHelpers& jit) { generateEnterStaticFunction(jit, CodeSpecializationKind::CodeForConstruct, Entry::CallLinkInfoForConstruct); }

// The call entry point of the NativeExecutable for bound functions whose target is a function, for use without a JIT. Equivalent to
// boundFunctionCallGenerator() (ThunkGenerators.cpp), but position-independent. To stack walkers the frame looks like a native
// function's. If there is no stack space for the target's frame, or the target has no code yet, it falls back to the executable's
// native function.
static void generateCallBoundFunction(CCallHelpers& jit)
{
    constexpr GPRReg bound = GPRInfo::regT0;
    constexpr GPRReg total = GPRInfo::regT1;
    constexpr GPRReg scratch = GPRInfo::regT2;
    constexpr GPRReg passed = GPRInfo::regT3;
    constexpr GPRReg value = GPRInfo::regT4;
    constexpr GPRReg vm = T10;
    CCallHelpers::JumpList slowCase;

    loadCalleeOfFrameBeingMadeAndItsVM(jit, bound, vm);
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

    // The call's own arguments come after the bound ones.
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

    CCallHelpers::JumpList haveArguments;
    haveArguments.append(jit.branchTest32(CCallHelpers::Zero, total));
    Jump areInFunction = jit.branch32(CCallHelpers::BelowOrEqual, total, TrustedImm32(JSBoundFunction::maxEmbeddedArgs));
    {
        jit.loadPtr(Address(bound, JSBoundFunction::offsetOfBoundArgs()), passed);
        CCallHelpers::Label next = jit.label();
        jit.sub32(TrustedImm32(1), total);
        jit.loadValue(CCallHelpers::BaseIndex(passed, total, CCallHelpers::TimesEight, JSCellButterfly::offsetOfData()), value);
        jit.storeValue(value, CCallHelpers::calleeArgumentSlot(1).indexedBy(total, CCallHelpers::TimesEight));
        jit.branchTest32(CCallHelpers::NonZero, total).linkTo(next, &jit);
        haveArguments.append(jit.jump());
    }
    areInFunction.link(&jit);
    {
        CCallHelpers::Label next = jit.label();
        jit.sub32(TrustedImm32(1), total);
        jit.loadValue(CCallHelpers::BaseIndex(bound, total, CCallHelpers::TimesEight, JSBoundFunction::offsetOfBoundArgs()), value);
        jit.storeValue(value, CCallHelpers::calleeArgumentSlot(1).indexedBy(total, CCallHelpers::TimesEight));
        jit.branchTest32(CCallHelpers::NonZero, total).linkTo(next, &jit);
    }
    haveArguments.link(&jit);

    jit.loadPtr(Address(bound, JSBoundFunction::offsetOfTargetFunction()), scratch);
    jit.storeValue(scratch, CCallHelpers::calleeFrameSlot(CallFrameSlot::callee));
    jit.loadPtr(Address(scratch, JSFunction::offsetOfExecutableOrRareData()), total);
    Jump hasExecutable = jit.branchTestPtr(CCallHelpers::Zero, total, TrustedImm32(JSFunction::rareDataTag));
    jit.loadPtr(Address(total, FunctionRareData::offsetOfExecutable() - JSFunction::rareDataTag), total);
    hasExecutable.link(&jit);
    // A short-form executable does not store this. See ExecutableBase::wayIntoShortForm().
    Jump saysHowToGetIn = jit.branchIfNotType(total, ShortFunctionExecutableType);
    jit.loadPtr(Address(total, FunctionExecutable::offsetOfAOTEntryFor(CodeSpecializationKind::CodeForCall)), scratch);
    slowCase.append(jit.branchTestPtr(CCallHelpers::Zero, scratch));
    jit.loadPtr(Address(vm, VM::offsetOfAOTRuntimeTable()), scratch);
    jit.loadPtr(Address(scratch, static_cast<unsigned>(Entry::EnterStaticFunctionForCall) * sizeof(void*)), scratch);
    Jump knowsHowToGetIn = jit.jump();
    saysHowToGetIn.link(&jit);
    jit.loadPtr(Address(total, ExecutableBase::offsetOfJITCodeWithArityCheckFor(CodeSpecializationKind::CodeForCall)), scratch);
    slowCase.append(jit.branchTestPtr(CCallHelpers::Zero, scratch));
    Jump isNative = jit.branchIfNotType(total, FunctionExecutableType);
    jit.loadPtr(Address(total, FunctionExecutable::offsetOfCodeBlockForCall()), passed);
    jit.storePtr(passed, CCallHelpers::calleeFrameCodeBlockBeforeCall());
    isNative.link(&jit);
    knowsHowToGetIn.link(&jit);
    jit.call(scratch, JSEntryPtrTag);
    jit.emitFunctionEpilogue();
    jit.ret();

    slowCase.link(&jit);
    jit.emitFunctionEpilogue();
    jit.loadPtr(Address(vm, VM::offsetOfAOTRuntimeTable()), T11);
    jit.loadPtr(Address(T11, static_cast<unsigned>(Entry::NativeCallTrampoline) * sizeof(void*)), T11);
    jit.farJump(T11, JSEntryPtrTag);
}

// The construct entry point of a static FunctionExecutable whose function was only compiled for call
// (FunctionExecutable::constructsByCalling()). Few such functions are ever used with `new`. To stack walkers the frame looks like a
// native function's.
static void generateConstructByCalling(CCallHelpers& jit)
{
    loadCalleeOfFrameBeingMadeAndItsVM(jit, T9, T10);
    jit.loadPtr(Address(T10, VM::offsetOfAOTRuntimeTable()), T11);
    jit.emitFunctionPrologue();
    jit.storePtr(CCallHelpers::TrustedImmPtr(nullptr), CCallHelpers::addressFor(CallFrameSlot::codeBlock));
    jit.subPtr(TrustedImm32(16), CCallHelpers::stackPointerRegister);
    jit.storePtr(T11, Address(CCallHelpers::stackPointerRegister));
    jit.move(GPRInfo::callFrameRegister, A0);
    jit.loadPtr(Address(T11, static_cast<unsigned>(Entry::operationAOTConstructByCalling) * sizeof(void*)), T9);
    jit.call(T9, OperationPtrTag);
    jit.loadPtr(Address(CCallHelpers::stackPointerRegister), T11);
    jit.addPtr(TrustedImm32(16), CCallHelpers::stackPointerRegister);
    // Returns the constructed object. A null result, returned together with the VM, means an exception was thrown.
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
        jit.move(CCallHelpers::linkRegister, A1);
    });
}

static void generateVirtualCall(CCallHelpers& jit) { dispatchCall(jit, CodeSpecializationKind::CodeForCall, [] { }); }
static void generateVirtualConstruct(CCallHelpers& jit) { dispatchCall(jit, CodeSpecializationKind::CodeForConstruct, [] { }); }
static void generateVirtualTailCall(CCallHelpers& jit) { dispatchCall(jit, CodeSpecializationKind::CodeForCall, [] { }); }

// calleeGPR = the callee. If it is a function with AOT code that has already been linked in this realm, leaves its EntryWord in
// T12. Clobbers T11 and T13.
static void findCodeOfCallee(CCallHelpers& jit, CodeSpecializationKind kind, CCallHelpers::JumpList& otherwise)
{
    otherwise.append(jit.branchIfNotCell(calleeGPR));
    otherwise.append(jit.branchIfNotType(calleeGPR, JSFunctionType));
    jit.loadPtr(Address(calleeGPR, JSFunction::offsetOfExecutableOrRareData()), T11);
    Jump hasExecutable = jit.branchTestPtr(CCallHelpers::Zero, T11, TrustedImm32(JSFunction::rareDataTag));
    jit.loadPtr(Address(T11, FunctionRareData::offsetOfExecutable() - JSFunction::rareDataTag), T11);
    hasExecutable.link(&jit);
    otherwise.append(jit.branchIfNotType(T11, JSTypeRange { FunctionExecutableType, ShortFunctionExecutableType }));
    jit.load64(Address(T11, FunctionExecutable::offsetOfAOTEntryFor(kind)), T12);
    Jump isNotStatic = jit.branchTest64(CCallHelpers::Zero, T12);
    jit.load32(Address(T11, FunctionExecutable::offsetOfAOTIndexFor(kind)), T13);
    if (kind == CodeSpecializationKind::CodeForConstruct)
        otherwise.append(jit.branch32(CCallHelpers::Equal, T13, TrustedImm32(static_cast<int32_t>(FunctionExecutable::aotIndexOfWhatConstructsByCalling))));
    jit.addPtr(TrustedImm32(Instance::offsetOfStates()), instanceGPR, T11);
    jit.load32(CCallHelpers::BaseIndex(T11, T13, CCallHelpers::TimesFour), T11);
    otherwise.append(jit.branch32(CCallHelpers::Below, T11, TrustedImm32(Instance::isLinkedWithoutData)));
    Jump isLinked = jit.jump();

    // The executable is not from the static heap, so its code was installed at run time (AOT::install()), which also linked it. The
    // callee runs with the caller's Instance, so it must belong to the same realm: another realm may have linked the same function.
    isNotStatic.link(&jit);
    otherwise.append(jit.branchIfNotType(T11, FunctionExecutableType));
    jit.loadPtr(Address(T11, ExecutableBase::offsetOfJITCodeFor(kind)), T11);
    otherwise.append(jit.branchTestPtr(CCallHelpers::Zero, T11));
    otherwise.append(jit.branch8(CCallHelpers::NotEqual, Address(T11, JSC::JITCode::offsetOfJITType()), TrustedImm32(static_cast<int32_t>(JITType::AOTJIT))));
    otherwise.append(jit.branchPtr(CCallHelpers::NotEqual, Address(T11, JITCode::offsetOfInstance()), instanceGPR));
    jit.load64(Address(T11, JITCode::offsetOfEntry()), T12);
    isLinked.link(&jit);
}

// Runs in the stub's own frame, with a standard frame set up below it except for its CodeBlock. Calls calleeGPR using the standard
// calling convention and returns its result to the stub's caller.
static void callTheWayTheEngineDoes(CCallHelpers& jit, CodeSpecializationKind kind)
{
    jit.move(calleeGPR, BaselineJITRegisters::Call::calleeGPR);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T11);
    jit.loadPtr(Address(T11, static_cast<unsigned>(kind == CodeSpecializationKind::CodeForCall ? Entry::CallLinkInfoForCall : Entry::CallLinkInfoForConstruct) * sizeof(void*)), BaselineJITRegisters::Call::callLinkInfoGPR);
    callStubFromStub(jit, kind == CodeSpecializationKind::CodeForCall ? Stub::VirtualCall : Stub::VirtualConstruct);
    jit.emitFunctionEpilogue();
    jit.ret();
}

static CCallHelpers::Label s_startOfCallOfAnyCount;

// Function.prototype.call.bind(f) ("uncurryThis"), which lets a program write f(object, ...) for object.f(...) regardless of what
// object.f has become. Calls f with the first argument as `this` and the rest as arguments. Without this it would be a call to a
// bound function, then to `call` (a JS builtin that collects its arguments in an array), then to f with the array spread.
static void callWhatCallIsBoundTo(CCallHelpers& jit)
{
    CCallHelpers::JumpList isNot;
    isNot.append(jit.branchIfNotCell(calleeGPR));
    jit.load32(Address(calleeGPR, JSCell::structureIDOffset()), T11);
    isNot.append(jit.branch32(CCallHelpers::NotEqual, T11, Address(instanceGPR, Instance::offsetOfStructureIDOfBoundFunctions())));
    jit.loadPtr(Address(calleeGPR, JSBoundFunction::offsetOfTargetFunction()), T11);
    isNot.append(jit.branchPtr(CCallHelpers::NotEqual, T11, Address(instanceGPR, Instance::offsetOfFunctionPrototypeCall())));
    isNot.append(jit.branchTest32(CCallHelpers::NonZero, Address(calleeGPR, JSBoundFunction::offsetOfBoundArgsLength())));
    // Code that reaches this point relies on the callee being a function, so the new callee must be one too. If `call` is bound to
    // anything else, the call proceeds as before.
    jit.load64(Address(calleeGPR, JSBoundFunction::offsetOfBoundThis()), T11);
    isNot.append(jit.branchIfNotCell(T11));
    isNot.append(jit.branchIfNotType(T11, JSFunctionType));
    jit.move(T11, calleeGPR);
    Jump nothingIsPassed = jit.branchTest32(CCallHelpers::Zero, countGPR);
    jit.move(argumentGPR(0), thisGPR);
    for (unsigned i = 0; i + 1 < numberOfArgumentGPRs; ++i)
        jit.move(argumentGPR(i + 1), argumentGPR(i));
    jit.sub32(TrustedImm32(1), countGPR);
    jit.jump().linkTo(s_startOfCallOfAnyCount, &jit);
    nothingIsPassed.link(&jit);
    jit.move(CCallHelpers::TrustedImm64(JSValue::ValueUndefined), thisGPR);
    jit.jump().linkTo(s_startOfCallOfAnyCount, &jit);
    isNot.link(&jit);
}

// See Stub::Call. count: the value of countGPR, if known statically.
static void generateCallTo(CCallHelpers& jit, CodeSpecializationKind kind, std::optional<unsigned> count)
{
    if (kind == CodeSpecializationKind::CodeForCall && !count)
        s_startOfCallOfAnyCount = jit.label();
    CCallHelpers::JumpList slowCase;
    findCodeOfCallee(jit, kind, slowCase);
    Jump takesList = jit.branchTest64(CCallHelpers::NonZero, T12, CCallHelpers::TrustedImm64(1LL << EntryWord::bitOfIsList));
    jit.urshift64(T12, TrustedImm32(EntryWord::shiftOfNumberOfParameters), T13);
    jit.and64(CCallHelpers::TrustedImm64(static_cast<int64_t>(EntryWord::addressMask)), T12);
    Jump enough = count ? jit.branch32(CCallHelpers::BelowOrEqual, T13, TrustedImm32(*count)) : jit.branch32(CCallHelpers::BelowOrEqual, T13, countGPR);
    // Passing more arguments than the function has parameters is harmless.
    jit.move(CCallHelpers::TrustedImm64(JSValue::ValueUndefined), T13);
    for (unsigned i = count.value_or(0); i < numberOfArgumentGPRs; ++i) {
        if (count)
            jit.move(T13, argumentGPR(i));
        else
            jit.moveConditionally32(CCallHelpers::Above, countGPR, TrustedImm32(i), argumentGPR(i), T13, argumentGPR(i));
    }
    enough.link(&jit);
    jit.farJump(T12, JSEntryPtrTag);

    // Store the arguments where the standard calling convention, and a Signature::List function, expect them.
    auto spill = [&] {
        unsigned registers = count.value_or(numberOfArgumentGPRs);
        jit.emitFunctionPrologue();
        jit.subPtr(TrustedImm32(WTF::roundUpToMultipleOf<stackAlignmentBytes()>((CallFrame::headerSizeInRegisters - CallerFrameAndPC::sizeInRegisters + 1 + registers) * sizeof(Register))), CCallHelpers::stackPointerRegister);
        jit.store64(calleeGPR, slotOfFrameBeingMade(CallFrameSlot::callee));
        jit.add32(TrustedImm32(1), countGPR, T11);
        jit.store64(T11, slotOfFrameBeingMade(CallFrameSlot::argumentCountIncludingThis));
        jit.store64(thisGPR, slotOfFrameBeingMade(CallFrameSlot::thisArgument));
        for (unsigned i = 0; i < registers; ++i)
            jit.store64(argumentGPR(i), slotOfFrameBeingMade(CallFrameSlot::thisArgument, (i + 1) * sizeof(Register)));
    };
    takesList.link(&jit);
    spill();
    jit.move(countGPR, argumentGPR(0));
    jit.addPtr(TrustedImm32(slotOfFrameBeingMade(CallFrameSlot::thisArgument, sizeof(Register)).offset), CCallHelpers::stackPointerRegister, argumentGPR(1));
    jit.and64(CCallHelpers::TrustedImm64(static_cast<int64_t>(EntryWord::addressMask)), T12);
    jit.call(T12, JSEntryPtrTag);
    jit.emitFunctionEpilogue();
    jit.ret();

    slowCase.link(&jit);
    if (kind == CodeSpecializationKind::CodeForCall)
        callWhatCallIsBoundTo(jit);
    spill();
    callTheWayTheEngineDoes(jit, kind);
}

// See Stub::CallList.
static void generateCallListTo(CCallHelpers& jit, CodeSpecializationKind kind)
{
    CCallHelpers::JumpList slowCase;
    findCodeOfCallee(jit, kind, slowCase);
    Jump takesRegisters = jit.branchTest64(CCallHelpers::Zero, T12, CCallHelpers::TrustedImm64(1LL << EntryWord::bitOfIsList));
    jit.and64(CCallHelpers::TrustedImm64(static_cast<int64_t>(EntryWord::addressMask)), T12);
    jit.farJump(T12, JSEntryPtrTag);

    takesRegisters.link(&jit);
    jit.and64(CCallHelpers::TrustedImm64(static_cast<int64_t>(EntryWord::addressMask)), T12);
    jit.move(argumentGPR(0), countGPR);
    jit.move(argumentGPR(1), T13);
    Jump noMore[numberOfArgumentGPRs];
    for (unsigned i = 0; i < numberOfArgumentGPRs; ++i) {
        noMore[i] = jit.branch32(CCallHelpers::BelowOrEqual, countGPR, TrustedImm32(i));
        jit.load64(Address(T13, i * sizeof(Register)), argumentGPR(i));
    }
    jit.farJump(T12, JSEntryPtrTag);
    for (unsigned i = 0; i < numberOfArgumentGPRs; ++i) {
        noMore[i].link(&jit);
        jit.move(CCallHelpers::TrustedImm64(JSValue::ValueUndefined), argumentGPR(i));
    }
    jit.farJump(T12, JSEntryPtrTag);

    slowCase.link(&jit);
    jit.emitFunctionPrologue();
    jit.move(argumentGPR(0), countGPR);
    jit.move(argumentGPR(1), T13);
    static_assert(stackAlignmentRegisters() == 2);
    jit.add32(TrustedImm32(CallFrame::headerSizeInRegisters - CallerFrameAndPC::sizeInRegisters + 1 + 1), countGPR, T11);
    jit.and32(TrustedImm32(~1), T11);
    jit.lshiftPtr(TrustedImm32(3), T11);
    jit.subPtr(CCallHelpers::stackPointerRegister, T11, T11);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfVM()), T12);
    Jump overflow = jit.branchPtr(CCallHelpers::Above, Address(T12, VM::offsetOfSoftStackLimit()), T11);
    Jump wrapped = jit.branchPtr(CCallHelpers::Above, T11, GPRInfo::callFrameRegister);
    jit.move(T11, CCallHelpers::stackPointerRegister);
    jit.store64(calleeGPR, slotOfFrameBeingMade(CallFrameSlot::callee));
    jit.add32(TrustedImm32(1), countGPR, T11);
    jit.store64(T11, slotOfFrameBeingMade(CallFrameSlot::argumentCountIncludingThis));
    jit.store64(thisGPR, slotOfFrameBeingMade(CallFrameSlot::thisArgument));
    jit.addPtr(TrustedImm32(slotOfFrameBeingMade(CallFrameSlot::thisArgument, sizeof(Register)).offset), CCallHelpers::stackPointerRegister, T11);
    Jump none = jit.branchTest32(CCallHelpers::Zero, countGPR);
    CCallHelpers::Label next = jit.label();
    jit.sub32(TrustedImm32(1), countGPR);
    jit.load64(CCallHelpers::BaseIndex(T13, countGPR, CCallHelpers::TimesEight), T12);
    jit.store64(T12, CCallHelpers::BaseIndex(T11, countGPR, CCallHelpers::TimesEight));
    jit.branchTest32(CCallHelpers::NonZero, countGPR).linkTo(next, &jit);
    none.link(&jit);
    callTheWayTheEngineDoes(jit, kind);

    overflow.link(&jit);
    wrapped.link(&jit);
    jit.emitFunctionEpilogue();
    throwStackOverflow(jit);
}

// See Stub::CallVarargs.
static void generateCallVarargsTo(CCallHelpers& jit, CodeSpecializationKind kind, bool inTailPosition = false)
{
    constexpr ptrdiff_t offsetOfCallee = -8;
    constexpr ptrdiff_t offsetOfThis = -16;
    constexpr ptrdiff_t offsetOfList = -24;
    constexpr ptrdiff_t offsetOfFirst = -32;
    constexpr ptrdiff_t offsetOfLength = -40;
    auto local = [](ptrdiff_t offset) { return Address(GPRInfo::callFrameRegister, offset); };
    auto callOperation = [&](Entry operation) {
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfGlobalObject()), A0);
        jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T11);
        jit.loadPtr(Address(T11, static_cast<unsigned>(operation) * sizeof(void*)), T11);
        jit.call(T11, OperationPtrTag);
    };
    CCallHelpers::JumpList exception;

    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(48), CCallHelpers::stackPointerRegister);
    jit.store64(calleeGPR, local(offsetOfCallee));
    jit.store64(thisGPR, local(offsetOfThis));
    jit.store64(argumentGPR(0), local(offsetOfList));
    jit.store64(argumentGPR(1), local(offsetOfFirst));

    // operation(globalObject, list, first) returns the number of arguments.
    jit.move(argumentGPR(1), A2);
    jit.move(argumentGPR(0), A1);
    callOperation(Entry::operationAOTSizeOfVarargs);
    exception.append(jit.branchTestPtr(CCallHelpers::NonZero, GPRInfo::returnValueGPR2));
    jit.store64(GPRInfo::returnValueGPR, local(offsetOfLength));

    static_assert(stackAlignmentRegisters() == 2);
    jit.add32(TrustedImm32(1), GPRInfo::returnValueGPR, T11);
    jit.and32(TrustedImm32(~1), T11);
    jit.lshiftPtr(TrustedImm32(3), T11);
    jit.subPtr(CCallHelpers::stackPointerRegister, T11, T11);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfVM()), T12);
    Jump overflow = jit.branchPtr(CCallHelpers::Above, Address(T12, VM::offsetOfSoftStackLimit()), T11);
    Jump wrapped = jit.branchPtr(CCallHelpers::Above, T11, GPRInfo::callFrameRegister);
    jit.move(T11, CCallHelpers::stackPointerRegister);

    // operation(globalObject, destination, list, first, count)
    jit.move(CCallHelpers::stackPointerRegister, A1);
    jit.load64(local(offsetOfList), A2);
    jit.load64(local(offsetOfFirst), A3);
    jit.load64(local(offsetOfLength), A4);
    callOperation(Entry::operationAOTLoadVarargs);
    exception.append(jit.branchTestPtr(CCallHelpers::NonZero, GPRInfo::returnValueGPR));

    jit.load64(local(offsetOfCallee), calleeGPR);
    jit.load64(local(offsetOfThis), thisGPR);
    jit.load64(local(offsetOfLength), argumentGPR(0));
    jit.move(CCallHelpers::stackPointerRegister, argumentGPR(1));
    if (inTailPosition) {
        // The arguments are moved over the frame of the function that called this stub, directly below that frame's header, which
        // doubles as the header of this stub's frame. If that function was itself called from such a stub frame, that frame is
        // overwritten too.
        constexpr GPRReg length = A0;
        constexpr GPRReg to = A1;
        constexpr GPRReg from = A2;
        constexpr GPRReg frame = A3;
        static_assert(length == argumentGPR(0) && to == argumentGPR(1));
        jit.move(argumentGPR(1), from);
        jit.loadPtr(Address(GPRInfo::callFrameRegister), frame);
        s_addressesOfLabels->append({ jit.label(), T11, s_whereCallVarargsIsReturnedTo });
        jit.m_assembler.adr(T11, 0);
        CCallHelpers::Label again = jit.label();
        jit.loadPtr(Address(frame, sizeof(void*)), T12);
        Jump isNotOfThisStub = jit.branchPtr(CCallHelpers::NotEqual, T12, T11);
        jit.loadPtr(Address(frame), frame);
        jit.jump().linkTo(again, &jit);
        isNotOfThisStub.link(&jit);
        jit.add64(TrustedImm32(1), length, T12);
        jit.and64(TrustedImm32(~1), T12);
        jit.lshift64(TrustedImm32(3), T12);
        jit.subPtr(frame, T12, to);
        // The destination is higher, so copy the last word first.
        jit.move(length, T12);
        Jump none = jit.branchTest64(CCallHelpers::Zero, T12);
        CCallHelpers::Label next = jit.label();
        jit.sub64(TrustedImm32(1), T12);
        jit.load64(CCallHelpers::BaseIndex(from, T12, CCallHelpers::TimesEight), T13);
        jit.store64(T13, CCallHelpers::BaseIndex(to, T12, CCallHelpers::TimesEight));
        jit.branchTest64(CCallHelpers::NonZero, T12).linkTo(next, &jit);
        none.link(&jit);
        jit.move(frame, GPRInfo::callFrameRegister);
        jit.move(to, CCallHelpers::stackPointerRegister);
        jit.jump().linkTo(s_callVarargsReturnAddress, &jit);
    } else {
        if (kind == CodeSpecializationKind::CodeForCall)
            s_callVarargsReturnAddress = jit.label();
        callStubFromStub(jit, kind == CodeSpecializationKind::CodeForCall ? Stub::CallList : Stub::ConstructList);
        if (kind == CodeSpecializationKind::CodeForCall)
            s_whereCallVarargsIsReturnedTo = jit.label();
        jit.emitFunctionEpilogue();
        jit.ret();
    }

    exception.link(&jit);
    jit.emitFunctionEpilogue();
    generateHandleException(jit);

    overflow.link(&jit);
    wrapped.link(&jit);
    jit.emitFunctionEpilogue();
    throwStackOverflow(jit);
}

static void generateCallVarargs(CCallHelpers& jit) { generateCallVarargsTo(jit, CodeSpecializationKind::CodeForCall); }
static void generateConstructVarargs(CCallHelpers& jit) { generateCallVarargsTo(jit, CodeSpecializationKind::CodeForConstruct); }
static void generateTailCallVarargs(CCallHelpers& jit) { generateCallVarargsTo(jit, CodeSpecializationKind::CodeForCall, true); }
static void generateCallList(CCallHelpers& jit) { generateCallListTo(jit, CodeSpecializationKind::CodeForCall); }
static void generateConstructList(CCallHelpers& jit) { generateCallListTo(jit, CodeSpecializationKind::CodeForConstruct); }

// Runs in the stub's own frame. A0 = an object, A1 = a slot for the named property. Leaves the property's value in A0. Uses the
// fast path of generateGetByIdWith(), then the operation, which is passed the name explicitly: there is no Site, and the link
// register does not identify one.
static void getWellKnownInFrameOfStub(CCallHelpers& jit, WellKnownIdentifier identifier, CCallHelpers::JumpList& exception)
{
    jit.load64(slotWord(A1, 0), T11);
    jit.load32(Address(A0, JSCell::structureIDOffset()), T12);
    Jump isOfAnotherStructure = jit.branch32(CCallHelpers::NotEqual, T11, T12);
    Jump isIndirect = jit.branchTest64(CCallHelpers::NonZero, T11, CCallHelpers::TrustedImm64(static_cast<int64_t>(Slot::isIndirect) << 32));
    jit.extractUnsignedBitfield64(T11, TrustedImm32(32), TrustedImm32(Slot::directLocationBits), T11);
    jit.load64(CCallHelpers::BaseIndex(A0, T11, CCallHelpers::TimesEight), A0);
    Jump found = jit.jump();

    isOfAnotherStructure.link(&jit);
    isIndirect.link(&jit);
    jit.move(A1, A3);
    jit.move(A0, A1);
    jit.move(TrustedImm32(static_cast<uint32_t>(identifier)), A2);
    loadInstance(jit, T11);
    jit.loadPtr(Address(T11, Instance::offsetOfGlobalObject()), A0);
    jit.loadPtr(Address(T11, Instance::offsetOfRuntimeTable()), T11);
    jit.loadPtr(Address(T11, static_cast<unsigned>(Entry::operationAOTGetByIdWellKnown) * sizeof(void*)), T11);
    jit.call(T11, OperationPtrTag);
    exception.append(jit.branchTestPtr(CCallHelpers::NonZero, GPRInfo::returnValueGPR2));
    found.link(&jit);
}

// Also runs in the stub's own frame. Calls calleeGPR with thisGPR as `this` and no arguments, checks that the result is an object,
// and leaves it in A0.
static void callForObjectInFrameOfStub(CCallHelpers& jit, CCallHelpers::JumpList& notObject)
{
    jit.move(TrustedImm32(0), countGPR);
    callStubFromStub(jit, Stub::Call);
    static_assert(GPRInfo::returnValueGPR == A0);
    notObject.append(jit.branchIfNotCell(A0));
    notObject.append(jit.branchIfNotObject(A0));
}

// The failure paths of the two helpers above. Still in the stub's frame.
static void throwFromFrameOfStub(CCallHelpers& jit, CCallHelpers::JumpList& notObject, CCallHelpers::JumpList& exception)
{
    notObject.link(&jit);
    loadInstance(jit, T11);
    jit.loadPtr(Address(T11, Instance::offsetOfGlobalObject()), A0);
    jit.loadPtr(Address(T11, Instance::offsetOfRuntimeTable()), T11);
    jit.loadPtr(Address(T11, static_cast<unsigned>(Entry::operationAOTThrowIteratorResultIsNotObject) * sizeof(void*)), T11);
    jit.call(T11, OperationPtrTag);
    exception.link(&jit);
    jit.emitFunctionEpilogue();
    loadInstance(jit, T9);
    jumpToEntry(jit, T9, Entry::HandleException);
}

static void generateIteratorNext(CCallHelpers& jit)
{
    CCallHelpers::JumpList generic;
    CCallHelpers::JumpList indexSlow;
    auto handled = [&] {
        jit.ret();
    };
    // value: the result of a fast-path operation, which is empty at the end of the iteration. Leaves `done` in A0.
    auto doneIfEmpty = [&](GPRReg value) {
        static_assert(JSValue::ValueTrue == JSValue::ValueFalse + 1);
        jit.compare64(CCallHelpers::Equal, value, TrustedImm32(0), A0);
        jit.add64(TrustedImm32(JSValue::ValueFalse), A0);
    };
    // Calls the operation with a stack word that survives the call, initialized to `kept`.
    auto callKeeping = [&](Entry operation, GPRReg kept, const auto& setUp) {
        jit.emitFunctionPrologue();
        jit.subPtr(TrustedImm32(16), CCallHelpers::stackPointerRegister);
        jit.store64(kept, Address(CCallHelpers::stackPointerRegister));
        loadInstance(jit, T11);
        setUp();
        jit.loadPtr(Address(T11, Instance::offsetOfGlobalObject()), A0);
        jit.loadPtr(Address(T11, Instance::offsetOfRuntimeTable()), T11);
        jit.loadPtr(Address(T11, static_cast<unsigned>(operation) * sizeof(void*)), T11);
        jit.call(T11, OperationPtrTag);
        jit.load64(Address(CCallHelpers::stackPointerRegister), A2);
        jit.emitFunctionEpilogue();
        Jump exception = jit.branchTestPtr(CCallHelpers::NonZero, GPRInfo::returnValueGPR2);
        jit.move(GPRInfo::returnValueGPR, A1);
        doneIfEmpty(A1);
        handled();
        exception.link(&jit);
        loadInstance(jit, T9);
        jumpToEntry(jit, T9, Entry::HandleException);
    };

    Jump nextIsCell = jit.branchIfCell(A0);

    // Only in this case may `iterator` be a sentinel rather than an object: `iterable` is then an array and `next` the index to
    // visit.
    generic.append(jit.branchIfNotCell(A1));
    generic.append(jit.branchIfNotType(A1, SentinelType));

    // An in-bounds, non-hole element in storage that holds JSValues. The end of the array, holes and everything else go to the
    // runtime.
    jit.move(CCallHelpers::TrustedImm64(JSValue::NumberTag), T9);
    indexSlow.append(jit.branch64(CCallHelpers::Below, A0, T9));
    indexSlow.append(jit.branchIfNotCell(A2));
    indexSlow.append(jit.branchIfNotType(A2, ArrayType));
    jit.load8(Address(A2, JSCell::indexingTypeAndMiscOffset()), T11);
    jit.and32(TrustedImm32(IndexingShapeMask), T11);
    Jump isInt32Shape = jit.branch32(CCallHelpers::Equal, T11, TrustedImm32(Int32Shape));
    indexSlow.append(jit.branch32(CCallHelpers::NotEqual, T11, TrustedImm32(ContiguousShape)));
    isInt32Shape.link(&jit);
    jit.loadPtr(Address(A2, JSObject::butterflyOffset()), T11);
    jit.load32(Address(T11, Butterfly::offsetOfPublicLength()), T12);
    // Unsigned comparison: the index of a finished iteration, -1, exceeds any length.
    indexSlow.append(jit.branch32(CCallHelpers::AboveOrEqual, A0, T12));
    indexSlow.append(jit.branch32(CCallHelpers::Equal, A0, TrustedImm32(std::numeric_limits<int32_t>::max())));
    jit.zeroExtend32ToWord(A0, T12);
    jit.load64(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesEight), T11);
    indexSlow.append(jit.branchTest64(CCallHelpers::Zero, T11));
    jit.move(T11, A1);
    jit.add32(TrustedImm32(1), T12);
    jit.or64(T9, T12, A2);
    jit.move(TrustedImm32(JSValue::ValueFalse), A0);
    handled();

    indexSlow.link(&jit);
    callKeeping(Entry::operationAOTIteratorNextWithIndex, A0, [&] {
        jit.move(A2, A1);
        jit.move(CCallHelpers::stackPointerRegister, A2);
    });

    nextIsCell.link(&jit);
    generic.append(jit.branchIfNotType(A0, SentinelType));
    callKeeping(Entry::operationAOTIteratorNextTryFast, A0, [&] { });

    // Generic case. result = next.call(iterator); done = result.done; value = done ? (unused) : result.value.
    generic.link(&jit);
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    constexpr int32_t keptNext = 0;
    constexpr int32_t keptSlots = 8;
    constexpr int32_t keptResult = 16;
    constexpr int32_t keptDone = 24;
    CCallHelpers::JumpList notObject;
    CCallHelpers::JumpList exception;
    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(32), sp);
    jit.store64(A0, Address(sp, keptNext));
    jit.storePtr(A3, Address(sp, keptSlots));
    jit.move(A1, thisGPR);
    jit.move(A0, calleeGPR);
    callForObjectInFrameOfStub(jit, notObject);
    jit.store64(A0, Address(sp, keptResult));
    jit.loadPtr(Address(sp, keptSlots), A1);
    getWellKnownInFrameOfStub(jit, WellKnownIdentifier::Done, exception);
    jit.store64(A0, Address(sp, keptDone));
    callStubFromStub(jit, Stub::ToBoolean);
    Jump isDone = jit.branchTest32(CCallHelpers::NonZero, A0);
    jit.load64(Address(sp, keptResult), A0);
    jit.loadPtr(Address(sp, keptSlots), A1);
    jit.addPtr(TrustedImm32(sizeof(Slot)), A1);
    getWellKnownInFrameOfStub(jit, WellKnownIdentifier::Value, exception);
    jit.move(A0, A1);
    Jump hasValue = jit.jump();
    isDone.link(&jit);
    jit.move(CCallHelpers::TrustedImm64(JSValue::ValueUndefined), A1);
    hasValue.link(&jit);
    jit.load64(Address(sp, keptDone), A0);
    jit.load64(Address(sp, keptNext), A2);
    jit.emitFunctionEpilogue();
    jit.ret();

    throwFromFrameOfStub(jit, notObject, exception);
}

// iterator = symbolIterator.call(iterable); next = iterator.next. If the runtime has a fast path for the iterable, `next` is a
// marker instead. See lowerIteratorOpen().
static void generateIteratorOpen(CCallHelpers& jit)
{
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    // An array with the realm's original Structure is iterated by index, with no iterator object. This matches the runtime's
    // IterationMode::FastArray.
    if (Options::useImmutableIntrinsics() && Options::useUnboxedFastArrayIteration()) {
        Jump isNotCell = jit.branchIfNotCell(A0);
        // Emitter::isOriginalArray()
        jit.load8(Address(A0, JSCell::indexingTypeAndMiscOffset()), T11);
        jit.urshift32(TrustedImm32(Instance::shiftOfKindOfArray), T11);
        jit.and32(TrustedImm32(Instance::numberOfKindsOfArray - 1), T11);
        loadInstance(jit, T12);
        jit.addPtr(TrustedImm32(Instance::offsetOfStructureIDsOfOriginalArrays()), T12, T13);
        jit.load32(CCallHelpers::BaseIndex(T13, T11, CCallHelpers::TimesFour), T11);
        jit.load32(Address(A0, JSCell::structureIDOffset()), T13);
        Jump isSomethingElse = jit.branch32(CCallHelpers::NotEqual, T11, T13);
        jit.loadPtr(Address(T12, Instance::offsetOfSentinelOfArrayIteration()), A0);
        jit.move(CCallHelpers::TrustedImm64(JSValue::encode(jsNumber(0))), A1);
        jit.ret();
        isNotCell.link(&jit);
        isSomethingElse.link(&jit);
    }

    constexpr int32_t keptNext = 0; // `next`, as returned by the operation.
    constexpr int32_t keptSlot = 8;
    constexpr int32_t keptIterable = 16; // Later reused for the iterator.
    constexpr int32_t keptSymbolIterator = 24;
    CCallHelpers::JumpList notObject;
    CCallHelpers::JumpList exception;
    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(32), sp);
    jit.storePtr(A2, Address(sp, keptSlot));
    jit.store64(A0, Address(sp, keptIterable));
    jit.store64(A1, Address(sp, keptSymbolIterator));
    jit.move(A1, A2);
    jit.move(A0, A1);
    jit.addPtr(TrustedImm32(keptNext), sp, A3);
    loadInstance(jit, T11);
    jit.loadPtr(Address(T11, Instance::offsetOfGlobalObject()), A0);
    jit.loadPtr(Address(T11, Instance::offsetOfRuntimeTable()), T11);
    jit.loadPtr(Address(T11, static_cast<unsigned>(Entry::operationAOTIteratorOpenTryFast) * sizeof(void*)), T11);
    jit.call(T11, OperationPtrTag);
    exception.append(jit.branchTestPtr(CCallHelpers::NonZero, GPRInfo::returnValueGPR2));
    Jump isGeneric = jit.branchTest64(CCallHelpers::Zero, A0);
    jit.load64(Address(sp, keptNext), A1);
    jit.emitFunctionEpilogue();
    jit.ret();

    isGeneric.link(&jit);
    jit.load64(Address(sp, keptIterable), thisGPR);
    jit.load64(Address(sp, keptSymbolIterator), calleeGPR);
    callForObjectInFrameOfStub(jit, notObject);
    jit.store64(A0, Address(sp, keptIterable));
    jit.loadPtr(Address(sp, keptSlot), A1);
    getWellKnownInFrameOfStub(jit, WellKnownIdentifier::Next, exception);
    jit.move(A0, A1);
    jit.load64(Address(sp, keptIterable), A0);
    jit.emitFunctionEpilogue();
    jit.ret();

    throwFromFrameOfStub(jit, notObject, exception);
}

static void generateIteratorCloseCheck(CCallHelpers& jit)
{
    Jump isNotCell = jit.branchIfNotCell(A0);
    Jump isNotMarked = jit.branchIfNotType(A0, SentinelType);
    // Lowering::inlineWatchpointSetIsStillValid()
    loadInstance(jit, T11);
    jit.loadPtr(Address(T11, Instance::offsetOfGlobalObject()), T12);
    jit.loadPtr(Address(T12, JSGlobalObject::offsetOfArrayIteratorProtocolWatchpointSet() + InlineWatchpointSet::offsetOfData()), T12);
    Jump isInvalidated = jit.branchPtr(CCallHelpers::Equal, T12, CCallHelpers::TrustedImmPtr(InlineWatchpointSet::encodeState(IsInvalidated)));
    Jump isThin = jit.branchTestPtr(CCallHelpers::NonZero, T12, TrustedImm32(InlineWatchpointSet::IsThinFlag));
    jit.load8(Address(T12, WatchpointSet::offsetOfState()), T12);
    Jump isInvalidatedToo = jit.branch32(CCallHelpers::Equal, T12, TrustedImm32(IsInvalidated));
    isThin.link(&jit);
    isNotCell.link(&jit);
    isNotMarked.link(&jit);
    jit.ret();

    // Array iterators have been given a `return` method, so closing one is observable and a real iterator is required.
    isInvalidated.link(&jit);
    isInvalidatedToo.link(&jit);
    jit.loadPtr(Address(T11, Instance::offsetOfGlobalObject()), A0);
    jit.loadPtr(Address(T11, Instance::offsetOfRuntimeTable()), T11);
    jit.loadPtr(Address(T11, static_cast<unsigned>(Entry::operationAOTMaterializeArrayIterator) * sizeof(void*)), T11);
    callAndCheckException(jit, T11, Returns::Value);
}

static void generateCall(CCallHelpers& jit) { generateCallTo(jit, CodeSpecializationKind::CodeForCall, std::nullopt); }
static void generateConstruct(CCallHelpers& jit) { generateCallTo(jit, CodeSpecializationKind::CodeForConstruct, std::nullopt); }
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

// The code below was written for A0 = the callee with `this` and the arguments in memory, and preserves all of that on every exit
// to `otherwise`. It is entered accordingly: the arguments (at most two) are saved in T14 and T15 and restored on the way out
// (generateCallIntrinsic()). thisGPR and calleeGPR are only read. The value returned is what the function would have returned.
//
// A1 = a Map or a Set, A2 = a key. Jumps to `otherwise` if the key, or a key it collides with, is of a kind this code does not
// handle; until then A0-A3 and T10 are preserved. Leaves the entry index of the key in A6, or jumps to `notThere`. The key's hash
// is in T13 when `checkCallee` is invoked, which happens before the table is read.
template<typename MapOrSet, typename CheckCallee>
static void findInMapOrSet(CCallHelpers& jit, CCallHelpers::JumpList& otherwise, CCallHelpers::JumpList& notThere, const CheckCallee& checkCallee)
{
    using Helper = typename MapOrSet::Helper;
    constexpr GPRReg key = A2;
    constexpr GPRReg data = A4;
    constexpr GPRReg count = A5;
    constexpr GPRReg slot = GPRInfo::argumentGPR6;
    constexpr GPRReg entryKey = GPRInfo::argumentGPR7;
    constexpr GPRReg deleted = T11;
    constexpr GPRReg hash = T13;
    constexpr GPRReg keyImpl = GPRInfo::regT14;
    constexpr GPRReg entryImpl = GPRInfo::regT15;
    constexpr GPRReg scratch = T12;
    constexpr GPRReg scratch2 = GPRInfo::regT8;

    // Compute the hash, if the key is already normalized. A number that is not an int32 may not be, and a BigInt is hashed by
    // value, not by address.
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
    hashed.append(jit.jump());
    notCell.link(&jit);
    Jump notNumber = jit.branchTest64(CCallHelpers::Zero, key, CCallHelpers::TrustedImm64(JSValue::NumberTag));
    otherwise.append(jit.branchIfNotInt32(key));
    notNumber.link(&jit);
    plain.link(&jit);
    jit.move(key, hash);
    jit.rapidHashMix64(hash, T11, scratch);
    hashed.link(&jit);

    checkCallee();

    jit.loadPtr(Address(A1, MapOrSet::offsetOfStorage()), data);
    notThere.append(jit.branchTestPtr(CCallHelpers::Zero, data));
    jit.addPtr(TrustedImm32(JSCellButterfly::offsetOfData()), data);
    jit.load32(Address(data, Helper::capacityIndex() * sizeof(uint64_t)), count);
    jit.sub32(TrustedImm32(1), count);
    jit.and32(hash, count);
    jit.add32(TrustedImm32(Helper::hashTableStartIndex()), count);
    jit.load64(CCallHelpers::BaseIndex(data, count, CCallHelpers::TimesEight), count);
    loadInstance(jit, deleted);
    jit.loadPtr(Address(deleted, Instance::offsetOfVM()), deleted);
    jit.loadPtr(Address(deleted, VM::offsetOfOrderedHashTableDeletedValue()), deleted);

    CCallHelpers::Label loop = jit.label();
    CCallHelpers::JumpList found;
    CCallHelpers::JumpList next;
    notThere.append(jit.branchTest64(CCallHelpers::Zero, count));
    jit.zeroExtend32ToWord(count, count);
    jit.getEffectiveAddress(CCallHelpers::BaseIndex(data, count, CCallHelpers::TimesEight), slot);
    jit.load64(Address(slot), entryKey);
    next.append(jit.branch64(CCallHelpers::Equal, entryKey, deleted));
    found.append(jit.branch64(CCallHelpers::Equal, entryKey, key));
    // Both keys are normalized. If they are not identical, they are the same key only if both are strings with equal contents.
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
    next.append(jit.branch32(CCallHelpers::NotEqual, scratch2, hash));
    jit.load32(Address(entryImpl, StringImpl::lengthMemoryOffset()), count);
    next.append(jit.branch32(CCallHelpers::NotEqual, count, Address(keyImpl, StringImpl::lengthMemoryOffset())));
    // Same hash and same length. Compare the characters, if both strings are 8-bit.
    otherwise.append(jit.branchTest32(CCallHelpers::Zero, scratch, TrustedImm32(StringImpl::flagIs8Bit())));
    otherwise.append(jit.branchTest32(CCallHelpers::Zero, Address(keyImpl, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.loadPtr(Address(entryImpl, StringImpl::dataOffset()), entryImpl);
    jit.loadPtr(Address(keyImpl, StringImpl::dataOffset()), scratch);
    CCallHelpers::Label compare = jit.label();
    found.append(jit.branchTest32(CCallHelpers::Zero, count));
    jit.sub32(TrustedImm32(1), count);
    jit.load8(CCallHelpers::BaseIndex(entryImpl, count, CCallHelpers::TimesOne), scratch2);
    jit.load8(CCallHelpers::BaseIndex(scratch, count, CCallHelpers::TimesOne), T9);
    jit.branch32(CCallHelpers::Equal, scratch2, T9).linkTo(compare, &jit);

    next.link(&jit);
    jit.load64(Address(slot, Helper::ChainOffset * sizeof(EncodedJSValue)), count);
    jit.jump().linkTo(loop, &jit);

    found.link(&jit);
}

static void generateCallIntrinsic(CCallHelpers& jit, StubIntrinsic intrinsic, CCallHelpers::Label operationVoidWithGlobalObject, CCallHelpers::JumpList& otherwise)
{
    auto argument = [](unsigned index) {
        RELEASE_ASSERT(index <= 2);
        return !index ? thisGPR : index == 1 ? T14 : T15;
    };
    jit.move(argumentGPR(0), T14);
    jit.move(argumentGPR(1), T15);
    jit.move(calleeGPR, A0);
    // Clobbers T11, T12.
    CCallHelpers::JumpList* ifNotTheCallee = &otherwise;
    auto checkCallee = [&](Entry function) {
        CCallHelpers::JumpList& otherwise = *ifNotTheCallee;
        otherwise.append(jit.branchIfNotCell(A0));
        otherwise.append(jit.branchIfNotType(A0, JSFunctionType));
        jit.loadPtr(Address(A0, JSFunction::offsetOfExecutableOrRareData()), T11);
        Jump hasExecutable = jit.branchTestPtr(CCallHelpers::Zero, T11, TrustedImm32(JSFunction::rareDataTag));
        jit.loadPtr(Address(T11, FunctionRareData::offsetOfExecutable() - JSFunction::rareDataTag), T11);
        hasExecutable.link(&jit);
        // Every kind of executable is large enough for this load to be in bounds, and only a NativeExecutable can hold a function's
        // address there.
        jit.loadPtr(Address(T11, NativeExecutable::offsetOfNativeFunctionFor(CodeSpecializationKind::CodeForCall)), T11);
        loadInstance(jit, T12);
        jit.loadPtr(Address(T12, Instance::offsetOfRuntimeTable()), T12);
        otherwise.append(jit.branchPtr(CCallHelpers::NotEqual, T11, Address(T12, static_cast<unsigned>(function) * sizeof(void*))));
    };
    auto loadThisOfType = [&](JSType type) {
        jit.move(argument(0), A1);
        otherwise.append(jit.branchIfNotCell(A1));
        otherwise.append(jit.branchIfNotType(A1, type));
    };
    auto returnInt32 = [&](GPRReg value) {
        jit.or64(CCallHelpers::TrustedImm64(JSValue::NumberTag), value, A0);
        jit.ret();
    };

    switch (intrinsic) {
    case StubIntrinsic::CharCodeAt:
    case StubIntrinsic::CodePointAt:
    case StubIntrinsic::CharAt: {
        // A resolved string and an in-bounds index.
        loadThisOfType(StringType);
        jit.loadPtr(Address(A1, JSString::offsetOfValue()), T13);
        otherwise.append(jit.branchTestPtr(CCallHelpers::NonZero, T13, TrustedImm32(JSString::isRopeInPointer)));
        jit.move(argument(1), A2);
        otherwise.append(jit.branchIfNotInt32(A2));
        checkCallee(intrinsic == StubIntrinsic::CharCodeAt ? Entry::HostStringCharCodeAt : intrinsic == StubIntrinsic::CodePointAt ? Entry::HostStringCodePointAt : Entry::HostStringCharAt);
        jit.zeroExtend32ToWord(A2, A2);
        jit.load32(Address(T13, StringImpl::lengthMemoryOffset()), A3);
        otherwise.append(jit.branch32(CCallHelpers::AboveOrEqual, A2, A3));
        jit.load32(Address(T13, StringImpl::flagsOffset()), A3);
        jit.loadPtr(Address(T13, StringImpl::dataOffset()), A4);
        Jump is16Bit = jit.branchTest32(CCallHelpers::Zero, A3, TrustedImm32(StringImpl::flagIs8Bit()));
        jit.load8(CCallHelpers::BaseIndex(A4, A2, CCallHelpers::TimesOne), A5);
        Jump haveCharacter = jit.jump();
        is16Bit.link(&jit);
        jit.load16(CCallHelpers::BaseIndex(A4, A2, CCallHelpers::TimesTwo), A5);
        if (intrinsic == StubIntrinsic::CodePointAt) {
            // A surrogate goes to the function.
            jit.sub32(A5, TrustedImm32(0xd800), A3);
            otherwise.append(jit.branch32(CCallHelpers::Below, A3, TrustedImm32(0x800)));
        }
        haveCharacter.link(&jit);
        if (intrinsic != StubIntrinsic::CharAt) {
            returnInt32(A5);
            break;
        }
        otherwise.append(jit.branch32(CCallHelpers::Above, A5, TrustedImm32(maxSingleCharacterString)));
        loadInstance(jit, T11);
        jit.loadPtr(Address(T11, Instance::offsetOfVM()), T11);
        jit.addPtr(TrustedImm32(OBJECT_OFFSETOF(VM, smallStrings) + SmallStrings::offsetOfSingleCharacterStrings()), T11);
        jit.loadPtr(CCallHelpers::BaseIndex(T11, A5, CCallHelpers::TimesEight), T11);
        otherwise.append(jit.branchTestPtr(CCallHelpers::Zero, T11));
        jit.move(T11, A0);
        jit.ret();
        break;
    }
    case StubIntrinsic::Push: {
        // Push onto an array with spare capacity, whose storage can hold the value as it is, where no write barrier is needed.
        loadThisOfType(ArrayType);
        jit.load8(Address(A1, JSCell::indexingTypeAndMiscOffset()), A3);
        jit.and32(TrustedImm32(IndexingShapeMask | CopyOnWrite), A3);
        jit.move(argument(1), A2);
        Jump contiguousCase = jit.branch32(CCallHelpers::Equal, A3, TrustedImm32(ContiguousShape));
        otherwise.append(jit.branch32(CCallHelpers::NotEqual, A3, TrustedImm32(Int32Shape)));
        otherwise.append(jit.branchIfNotInt32(A2));
        Jump isInt32 = jit.jump();
        contiguousCase.link(&jit);
        Jump notCell = jit.branchIfNotCell(A2);
        jit.load8(Address(A1, JSCell::cellStateOffset()), T11);
        loadInstance(jit, T12);
        jit.loadPtr(Address(T12, Instance::offsetOfVM()), T12);
        otherwise.append(jit.branch32(CCallHelpers::BelowOrEqual, T11, Address(T12, VM::offsetOfHeapBarrierThreshold())));
        notCell.link(&jit);
        isInt32.link(&jit);
        checkCallee(Entry::HostArrayPush);
        jit.loadPtr(Address(A1, JSObject::butterflyOffset()), A4);
        jit.load32(Address(A4, Butterfly::offsetOfPublicLength()), A5);
        otherwise.append(jit.branch32(CCallHelpers::AboveOrEqual, A5, Address(A4, Butterfly::offsetOfVectorLength())));
        jit.store64(A2, CCallHelpers::BaseIndex(A4, A5, CCallHelpers::TimesEight));
        jit.add32(TrustedImm32(1), A5);
        jit.store32(A5, Address(A4, Butterfly::offsetOfPublicLength()));
        returnInt32(A5);
        break;
    }
    case StubIntrinsic::Pop: {
        // Pop the last element of an array, if it is not a hole.
        loadThisOfType(ArrayType);
        jit.load8(Address(A1, JSCell::indexingTypeAndMiscOffset()), A3);
        jit.and32(TrustedImm32(IndexingShapeMask | CopyOnWrite), A3);
        Jump contiguousCase = jit.branch32(CCallHelpers::Equal, A3, TrustedImm32(ContiguousShape));
        otherwise.append(jit.branch32(CCallHelpers::NotEqual, A3, TrustedImm32(Int32Shape)));
        contiguousCase.link(&jit);
        checkCallee(Entry::HostArrayPop);
        jit.loadPtr(Address(A1, JSObject::butterflyOffset()), A4);
        jit.load32(Address(A4, Butterfly::offsetOfPublicLength()), A5);
        otherwise.append(jit.branchTest32(CCallHelpers::Zero, A5));
        jit.sub32(TrustedImm32(1), A5);
        otherwise.append(jit.branch32(CCallHelpers::AboveOrEqual, A5, Address(A4, Butterfly::offsetOfVectorLength())));
        jit.load64(CCallHelpers::BaseIndex(A4, A5, CCallHelpers::TimesEight), GPRInfo::argumentGPR6);
        otherwise.append(jit.branchTest64(CCallHelpers::Zero, GPRInfo::argumentGPR6));
        jit.store64(CCallHelpers::TrustedImm64(JSValue::encode(JSValue())), CCallHelpers::BaseIndex(A4, A5, CCallHelpers::TimesEight));
        jit.store32(A5, Address(A4, Butterfly::offsetOfPublicLength()));
        jit.move(GPRInfo::argumentGPR6, A0);
        jit.ret();
        break;
    }
    case StubIntrinsic::IsArray: {
        jit.move(argument(1), A1);
        checkCallee(Entry::HostArrayIsArray);
        Jump notCell = jit.branchIfNotCell(A1);
        jit.load8(Address(A1, JSCell::typeInfoTypeOffset()), A2);
        // For a proxy the answer depends on its target, which the function determines.
        otherwise.append(jit.branch32(CCallHelpers::Equal, A2, TrustedImm32(ProxyObjectType)));
        static_assert(ArrayType + 1 == DerivedArrayType);
        jit.sub32(TrustedImm32(ArrayType), A2);
        Jump notArray = jit.branch32(CCallHelpers::Above, A2, TrustedImm32(DerivedArrayType - ArrayType));
        jit.move(CCallHelpers::TrustedImm64(JSValue::encode(jsBoolean(true))), A0);
        jit.ret();
        notCell.link(&jit);
        notArray.link(&jit);
        jit.move(CCallHelpers::TrustedImm64(JSValue::encode(jsBoolean(false))), A0);
        jit.ret();
        break;
    }
    case StubIntrinsic::Get:
    case StubIntrinsic::Has:
    case StubIntrinsic::Set:
    case StubIntrinsic::SetIgnoringResult:
    case StubIntrinsic::Add:
    case StubIntrinsic::AddIgnoringResult: {
        bool isOfMap = intrinsic == StubIntrinsic::Get || intrinsic == StubIntrinsic::Set || intrinsic == StubIntrinsic::SetIgnoringResult;
        bool isOfSet = intrinsic == StubIntrinsic::Add || intrinsic == StubIntrinsic::AddIgnoringResult;
        // findInMapOrSet() uses the registers that hold `this` and the arguments, and preserves these.
        CCallHelpers::JumpList putBack;
        ifNotTheCallee = &putBack;
        bool hasValue = intrinsic == StubIntrinsic::Set || intrinsic == StubIntrinsic::SetIgnoringResult;
        jit.move(argument(0), A1);
        jit.move(argument(1), A2);
        if (hasValue)
            jit.move(argument(2), A3);
        putBack.append(jit.branchIfNotCell(A1));
        Jump isSet;
        if (isOfSet)
            putBack.append(jit.branchIfNotType(A1, JSSetType));
        else {
            if (!isOfMap)
                isSet = jit.branchIfType(A1, JSSetType);
            putBack.append(jit.branchIfNotType(A1, JSMapType));
        }

        auto returnValue = [&](JSValue value) {
            jit.move(CCallHelpers::TrustedImm64(JSValue::encode(value)), A0);
            jit.ret();
        };
        auto leaveToOperation = [&](Entry operation) {
            jit.move(TrustedImm32(static_cast<unsigned>(operation) * sizeof(void*)), T9);
            jit.jump().linkTo(operationVoidWithGlobalObject, &jit);
        };
        if (!isOfSet) {
            CCallHelpers::JumpList notThere;
            findInMapOrSet<JSMap>(jit, putBack, notThere, [&] {
                checkCallee(intrinsic == StubIntrinsic::Get ? Entry::HostMapGet : intrinsic == StubIntrinsic::Has ? Entry::HostMapHas : Entry::HostMapSet);
            });
            switch (intrinsic) {
            case StubIntrinsic::Get:
                jit.load64(Address(GPRInfo::argumentGPR6, sizeof(EncodedJSValue)), A0);
                jit.ret();
                notThere.link(&jit);
                returnValue(jsUndefined());
                break;
            case StubIntrinsic::Has:
                returnValue(jsBoolean(true));
                notThere.link(&jit);
                returnValue(jsBoolean(false));
                break;
            default: {
                // Overwrite the existing value, if no write barrier is needed.
                Jump notCell = jit.branchIfNotCell(A3);
                jit.loadPtr(Address(A1, JSMap::offsetOfStorage()), T11);
                jit.load8(Address(T11, JSCell::cellStateOffset()), T11);
                loadInstance(jit, T12);
                jit.loadPtr(Address(T12, Instance::offsetOfVM()), T12);
                putBack.append(jit.branch32(CCallHelpers::BelowOrEqual, T11, Address(T12, VM::offsetOfHeapBarrierThreshold())));
                notCell.link(&jit);
                jit.store64(A3, Address(GPRInfo::argumentGPR6, sizeof(EncodedJSValue)));
                jit.move(A1, A0);
                jit.ret();
                notThere.link(&jit);
                if (intrinsic == StubIntrinsic::Set)
                    putBack.append(jit.jump());
                else {
                    jit.move(T13, A4);
                    leaveToOperation(Entry::operationAOTMapSet);
                }
                break;
            }
            }
        }
        if (isSet.isSet())
            isSet.link(&jit);
        if (!isOfMap) {
            CCallHelpers::JumpList notThere;
            findInMapOrSet<JSSet>(jit, putBack, notThere, [&] {
                checkCallee(intrinsic == StubIntrinsic::Has ? Entry::HostSetHas : Entry::HostSetAdd);
            });
            if (intrinsic == StubIntrinsic::Has) {
                returnValue(jsBoolean(true));
                notThere.link(&jit);
                returnValue(jsBoolean(false));
            } else {
                jit.move(A1, A0);
                jit.ret();
                notThere.link(&jit);
                if (intrinsic == StubIntrinsic::Add)
                    putBack.append(jit.jump());
                else {
                    jit.move(T13, A3);
                    leaveToOperation(Entry::operationAOTSetAdd);
                }
            }
        }
        putBack.link(&jit);
        jit.move(A1, thisGPR);
        jit.move(A2, T14);
        if (hasValue)
            jit.move(A3, T15);
        otherwise.append(jit.jump());
        break;
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

StubIntrinsic stubIntrinsicFor(UniquedStringImpl* name, unsigned argumentCountIncludingThis, bool usesResult)
{
    enum { Any, Wanted, ResultUnused };
    if constexpr (!usesStubs)
        return StubIntrinsic::None;
    if (name->length() > 11 || name->isSymbol())
        return StubIntrinsic::None;
#define AOT_STUB_INTRINSIC_FOR(intrinsic, text, argumentCount, result) \
    if (argumentCount + 1 == argumentCountIncludingThis && (result == Any || (result == Wanted) == usesResult) && WTF::equal(name, text ""_s)) \
        return StubIntrinsic::intrinsic;
    FOR_EACH_AOT_STUB_INTRINSIC(AOT_STUB_INTRINSIC_FOR)
#undef AOT_STUB_INTRINSIC_FOR
    return StubIntrinsic::None;
}


// Called like the operation, with the same arguments. Tries the helper first, with the arguments rearranged by setUp(). If the
// helper bails out, calls the operation with its original arguments.
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
    jit.move(TrustedImm32(0), GPRInfo::returnValueGPR2); // No exception.
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

// The helper takes the operation's arguments without the leading global object.
static void generateAheadOf(CCallHelpers& jit, Entry operation, Stub helper, unsigned argumentsOfHelper)
{
    generateAheadOf(jit, operation, [&] {
        for (unsigned i = 0; i < argumentsOfHelper; ++i)
            jit.move(GPRInfo::toArgumentRegister(i + 1), GPRInfo::toArgumentRegister(i));
        callStubFromStub(jit, helper);
    });
}

// (globalObject, values, count, indexingType)
static void generateAheadOfNewArray(CCallHelpers& jit)
{
    generateAheadOf(jit, Entry::BehindNewArray, [&] {
        jit.move(A1, A0);
        jit.move(A2, A1);
        Jump areInt32 = jit.branch32(CCallHelpers::Equal, A3, TrustedImm32(ArrayWithInt32));
        callStubFromStub(jit, Stub::HelperNewArray);
        Jump done = jit.jump();
        areInt32.link(&jit);
        callStubFromStub(jit, Stub::HelperNewArrayOfInt32);
        done.link(&jit);
    });
}

// (globalObject, count, arguments, skipped)
static void generateAheadOfCreateRest(CCallHelpers& jit)
{
    generateAheadOf(jit, Entry::BehindCreateRest, [&] {
        jit.zeroExtend32ToWord(A3, A3);
        jit.getEffectiveAddress(CCallHelpers::BaseIndex(A2, A3, CCallHelpers::TimesEight), A0);
        Jump some = jit.branch32(CCallHelpers::Above, A1, A3);
        jit.move(A3, A1);
        some.link(&jit);
        jit.sub32(A3, A1);
        callStubFromStub(jit, Stub::HelperNewArray);
    });
}

static void generateAheadOfNewArrayBuffer(CCallHelpers& jit) { generateAheadOf(jit, Entry::BehindNewArrayBuffer, Stub::HelperNewArrayBuffer, 1); }
static void generateAheadOfNewArrayWithSpread(CCallHelpers& jit) { generateAheadOf(jit, Entry::BehindNewArrayWithSpread, Stub::HelperNewArrayWithSpread, 3); }
static void generateAheadOfNewArrayWithSpecies(CCallHelpers& jit) { generateAheadOf(jit, Entry::BehindNewArrayWithSpecies, Stub::HelperNewArrayWithSpecies, 2); }
static void generateAheadOfCreateLexicalEnvironment(CCallHelpers& jit) { generateAheadOf(jit, Entry::BehindCreateLexicalEnvironment, Stub::HelperNewActivation, 4); }
static void generateAheadOfMakeRope2(CCallHelpers& jit) { generateAheadOf(jit, Entry::BehindMakeRope2, Stub::HelperMakeRope2, 2); }
static void generateAheadOfMakeRope3(CCallHelpers& jit) { generateAheadOf(jit, Entry::BehindMakeRope3, Stub::HelperMakeRope3, 3); }
static void generateAheadOfStringSliceWithEnd(CCallHelpers& jit) { generateAheadOf(jit, Entry::BehindStringSliceWithEnd, Stub::HelperStringSlice, 3); }
static void generateAheadOfStringSubstringWithEnd(CCallHelpers& jit) { generateAheadOf(jit, Entry::BehindStringSubstringWithEnd, Stub::HelperStringSubstring, 3); }
static void generateAheadOfToLowerCase(CCallHelpers& jit) { generateAheadOf(jit, Entry::BehindToLowerCase, Stub::HelperToLowerCase, 1); }
static void generateAheadOfValueAdd(CCallHelpers& jit) { generateAheadOf(jit, Entry::BehindValueAdd, Stub::HelperAddStrings, 2); }
static void generateAheadOfObjectKeysObject(CCallHelpers& jit) { generateAheadOf(jit, Entry::BehindObjectKeysObject, Stub::HelperObjectKeys, 1); }

#define AOT_GENERATE_HELPER(name) static void generate##name(CCallHelpers& jit) { generateHelper(jit, Stub::name); }
FOR_EACH_AOT_HELPER(AOT_GENERATE_HELPER)
#undef AOT_GENERATE_HELPER

#else // CPU(ARM64)

#define AOT_NO_STUB(name) static void generate##name(CCallHelpers& jit) { jit.breakpoint(); }
FOR_EACH_AOT_STUB(AOT_NO_STUB)
#undef AOT_NO_STUB

#endif // CPU(ARM64)

// The stubs that call an operation take its offset in the runtime table in T9. The stubs that call a function take the argument
// count.
static constexpr Stub operationCallStubs[] = {
    Stub::OperationValue, Stub::OperationVoid, Stub::OperationDouble, Stub::OperationValueWithGlobalObject, Stub::OperationVoidWithGlobalObject, Stub::OperationDoubleWithGlobalObject,
    Stub::PlainOperation, Stub::PlainOperationWithGlobalObject, Stub::PlainOperationWithVM,
    Stub::ColdOperationVoid, Stub::ColdOperationVoidOfLeaf, Stub::ColdOperationValue, Stub::ColdOperationValueOfLeaf,
};
static constexpr Stub functionCallStubs[] = { Stub::Call, Stub::Construct };
static constexpr unsigned numberOfCountsWithThunk = numberOfArgumentGPRs + 1; // From zero up to the number of argument registers.
// The prologue stub takes the frame size, which is a multiple of this.
static constexpr unsigned unitOfFrameSize = stackAlignmentBytes();
static constexpr unsigned biggestFrameWithThunk = 64 * unitOfFrameSize;
static constexpr unsigned firstThunkOfCalls = std::size(operationCallStubs) * numberOfEntries;
static constexpr unsigned firstThunkOfPrologue = firstThunkOfCalls + std::size(functionCallStubs) * numberOfCountsWithThunk;
static constexpr unsigned firstThunkOfIntrinsics = firstThunkOfPrologue + biggestFrameWithThunk / unitOfFrameSize;
// acceptsOperandInAnyRegister(). These stubs have a copy of their fast path for each register...
static constexpr Stub stubsWithAnyRegisterOperand[] = { Stub::WriteBarrier, Stub::ToBoolean, Stub::GetById, Stub::GetByIdWellKnown,
    // ...and these are entered through a move.
    Stub::PutById, Stub::GetByVal, Stub::GetByValAtIndex, Stub::PutByVal, Stub::PutByValAtIndex, Stub::PutByValDirect, Stub::GetFromScope, Stub::GetGlobal, Stub::ResolveScope,
    Stub::StrictEqual, Stub::LooseEqual, Stub::IsStringEqualTo, Stub::GetLength, Stub::HelperAddField,
    Stub::ReadSlot0, Stub::ReadSlot1, Stub::ReadSlot2, Stub::ReadSlot3, Stub::ReadSlot4, Stub::ReadSlot5, Stub::ReadSlot6, Stub::ReadSlot7,
    Stub::ReadSlotOrUndefined0, Stub::ReadSlotOrUndefined1, Stub::ReadSlotOrUndefined2, Stub::ReadSlotOrUndefined3, Stub::ReadSlotOrUndefined4, Stub::ReadSlotOrUndefined5, Stub::ReadSlotOrUndefined6, Stub::ReadSlotOrUndefined7 };
// So are these operations, which are already entered through a thunk that sets T9.
struct OperationWithAnyRegisterOperand {
    Stub stub;
    Entry operation;
};
static constexpr OperationWithAnyRegisterOperand operationsWithAnyRegisterOperand[] = {
    { Stub::ColdOperationVoid, Entry::operationAOTCheckType }, { Stub::ColdOperationVoidOfLeaf, Entry::operationAOTCheckType },
    { Stub::ColdOperationVoid, Entry::operationAOTCheckTypedLayout }, { Stub::ColdOperationVoidOfLeaf, Entry::operationAOTCheckTypedLayout },
    { Stub::ColdOperationValue, Entry::operationAOTGetElementOrEmpty }, { Stub::ColdOperationValueOfLeaf, Entry::operationAOTGetElementOrEmpty },
    { Stub::ColdOperationValue, Entry::operationAOTGetByVal }, { Stub::ColdOperationValueOfLeaf, Entry::operationAOTGetByVal },
    { Stub::OperationValueWithGlobalObject, Entry::operationAOTNewFunction },
    { Stub::OperationValueWithGlobalObject, Entry::operationAOTToString },
    { Stub::OperationValueWithGlobalObject, Entry::operationAOTToThis },
    { Stub::OperationValueWithGlobalObject, Entry::operationAOTCreateLexicalEnvironment },
    { Stub::OperationValueWithGlobalObject, Entry::operationAOTCloneObject },
    { Stub::OperationValueWithGlobalObject, Entry::operationAOTReadLazyClosureVar },
    { Stub::OperationVoidWithGlobalObject, Entry::operationAOTThrow },
    { Stub::OperationVoidWithGlobalObject, Entry::operationAOTThrowNotAFunction },
    { Stub::PlainOperationWithGlobalObject, Entry::operationAOTToBoolean },
};
// x0 to x29. Not all of them can hold an operand, but a pinned register can if the operand equals its value: the boxed integer 0 is
// the number tag.
static constexpr unsigned numberOfRegistersForOperand = 30;
static constexpr unsigned firstThunkOfOperands = firstThunkOfIntrinsics + numberOfStubIntrinsics;
static constexpr unsigned firstThunkOfOperandsOfOperations = firstThunkOfOperands + std::size(stubsWithAnyRegisterOperand) * numberOfRegistersForOperand;
// acceptsTwoOperandsInAnyRegisters(): entered through two moves.
static constexpr Stub stubsWithTwoAnyRegisterOperands[] = { Stub::StrictEqual, Stub::LooseEqual, Stub::PutById, Stub::GetByVal, Stub::GetByValAtIndex, Stub::PutByVal, Stub::PutByValAtIndex, Stub::PutByValDirect, Stub::HelperAddField };
static constexpr unsigned numberOfRegistersForPair = 16; // x0 to x8, x19 to x25
// returnsResultInAnyRegister(). All are variants of Stub::OperationValueWithGlobalObject, with one copy per result register.
struct OperationWithAnyRegisterResult {
    Entry operation;
    bool acceptsOperandInAnyRegister; // Also listed in operationsWithAnyRegisterOperand.
};
static constexpr OperationWithAnyRegisterResult operationsWithAnyRegisterResult[] = {
    { Entry::operationAOTNewFunction, true }, { Entry::operationAOTToString, true }, { Entry::operationAOTToThis, true }, { Entry::operationAOTCreateLexicalEnvironment, true },
    { Entry::operationAOTCloneObject, true }, { Entry::operationAOTReadLazyClosureVar, true },
    { Entry::operationAOTNewObjectLiteral, false }, { Entry::operationAOTNewInternalFieldObject, false }, { Entry::operationAOTNewObject, false }, { Entry::operationAOTNewArray, false },
    { Entry::operationAOTNewArrayWithSpecies, false }, { Entry::operationMakeRope2, false }, { Entry::operationMakeRope3, false },
};
static constexpr unsigned numberOfRegistersForResult = 7; // x19 to x25
static constexpr unsigned firstThunkOfPairs = firstThunkOfOperandsOfOperations + std::size(operationsWithAnyRegisterOperand) * numberOfRegistersForOperand;
static constexpr unsigned firstThunkOfResults = firstThunkOfPairs + std::size(stubsWithTwoAnyRegisterOperands) * numberOfRegistersForPair * numberOfRegistersForPair;
static constexpr unsigned numberOfThunks = firstThunkOfResults + std::size(operationsWithAnyRegisterResult) * numberOfRegistersForResult * numberOfRegistersForOperand;
static_assert(numberOfThunks < std::numeric_limits<uint16_t>::max());

static bool thereAreNoWaysInByRegister()
{
    return !usesStubs;
}

static std::optional<unsigned> firstThunkForOperandOf(Stub stub, std::optional<uint32_t> valueOfT9)
{
    if (thereAreNoWaysInByRegister())
        return std::nullopt;
    if (!valueOfT9) {
        for (unsigned i = 0; i < std::size(stubsWithAnyRegisterOperand); ++i) {
            if (stubsWithAnyRegisterOperand[i] == stub)
                return firstThunkOfOperands + i * numberOfRegistersForOperand;
        }
        return std::nullopt;
    }
    for (unsigned i = 0; i < std::size(operationsWithAnyRegisterOperand); ++i) {
        if (operationsWithAnyRegisterOperand[i].stub == stub && static_cast<unsigned>(operationsWithAnyRegisterOperand[i].operation) * sizeof(void*) == *valueOfT9)
            return firstThunkOfOperandsOfOperations + i * numberOfRegistersForOperand;
    }
    return std::nullopt;
}

bool acceptsOperandInAnyRegister(Stub stub, std::optional<uint32_t> valueOfT9)
{
    return !!firstThunkForOperandOf(stub, valueOfT9);
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
    // All of the above take the global object as their first argument, which the stub supplies.
    return callsOperation(stub) ? GPRInfo::argumentGPR1 : GPRInfo::argumentGPR0;
}

bool preservesOperandRegister(Stub stub)
{
    return stub == Stub::WriteBarrier;
}

bool operandMayBeIn(Stub stub, GPRReg reg)
{
#if CPU(ARM64)
    unsigned number = static_cast<unsigned>(reg) - static_cast<unsigned>(ARM64Registers::x0);
    if (number >= numberOfRegistersForOperand || number == 16 || number == 17) // The assembler's scratch registers.
        return false;
    // The entry point starts with a move.
    if (callsOperation(stub))
        return true;
    switch (stub) {
    case Stub::WriteBarrier:
    case Stub::ToBoolean:
        return number < 9 || number > 11; // T9 to T11
    case Stub::GetById:
    case Stub::GetByIdWellKnown:
        return (number < 9 && reg != GPRInfo::argumentGPR1) || number > 15; // A1 holds the slot. T9 to T15 are clobbered.
    default:
        if (slotReadBy(stub))
            return (number < 9 && reg != GPRInfo::argumentGPR1) || number > 15; // A1 holds the field ID. T9 to T15 are clobbered.
        return true;
    }
#else
    UNUSED_PARAM(stub);
    UNUSED_PARAM(reg);
    return false;
#endif
}

unsigned thunkForOperandIn(Stub stub, std::optional<uint32_t> valueOfT9, GPRReg reg)
{
    RELEASE_ASSERT(operandMayBeIn(stub, reg));
    return *firstThunkForOperandOf(stub, valueOfT9) + static_cast<unsigned>(reg);
}

bool acceptsTwoOperandsInAnyRegisters(Stub stub)
{
    if (thereAreNoWaysInByRegister())
        return false;
    for (Stub other : stubsWithTwoAnyRegisterOperands) {
        if (other == stub)
            return true;
    }
    return false;
}

// The register's index among those that have entry points, if any.
static std::optional<unsigned> numberAmongRegistersForPair(GPRReg reg)
{
    unsigned number = static_cast<unsigned>(reg);
    if (number <= 8)
        return number;
    if (number >= 19 && number <= 25)
        return number - 10;
    return std::nullopt;
}

static GPRReg registerForPair(unsigned number)
{
    return static_cast<GPRReg>(number <= 8 ? number : number + 10);
}

static std::optional<unsigned> indexOfOperationWithAnyRegisterResult(Stub stub, std::optional<uint32_t> valueOfT9)
{
    if (thereAreNoWaysInByRegister() || stub != Stub::OperationValueWithGlobalObject || !valueOfT9)
        return std::nullopt;
    for (unsigned i = 0; i < std::size(operationsWithAnyRegisterResult); ++i) {
        if (static_cast<unsigned>(operationsWithAnyRegisterResult[i].operation) * sizeof(void*) == *valueOfT9)
            return i;
    }
    return std::nullopt;
}

bool returnsResultInAnyRegister(Stub stub, std::optional<uint32_t> valueOfT9)
{
    return !!indexOfOperationWithAnyRegisterResult(stub, valueOfT9);
}

std::optional<unsigned> thunkFor(Stub stub, uint32_t valueOfT9)
{
    if constexpr (!usesStubs)
        return std::nullopt;
    for (unsigned i = 0; i < std::size(operationCallStubs); ++i) {
        if (operationCallStubs[i] == stub)
            return valueOfT9 % sizeof(void*) || valueOfT9 / sizeof(void*) >= numberOfEntries ? std::nullopt : std::optional<unsigned> { i * numberOfEntries + valueOfT9 / sizeof(void*) };
    }
    for (unsigned i = 0; i < std::size(functionCallStubs); ++i) {
        if (functionCallStubs[i] == stub)
            return valueOfT9 >= numberOfCountsWithThunk ? std::nullopt : std::optional<unsigned> { firstThunkOfCalls + i * numberOfCountsWithThunk + valueOfT9 };
    }
    if (stub == Stub::Prologue && valueOfT9 && valueOfT9 <= biggestFrameWithThunk && !(valueOfT9 % unitOfFrameSize))
        return firstThunkOfPrologue + valueOfT9 / unitOfFrameSize - 1;
    if (stub == Stub::CallIntrinsic) {
        RELEASE_ASSERT(valueOfT9 && valueOfT9 <= numberOfStubIntrinsics);
        return firstThunkOfIntrinsics + valueOfT9 - 1;
    }
    return std::nullopt;
}

// Moves `first` to A0 and `second` to A1, handling the case where either is already in A0 or A1.
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

const StubBlob& stubBlob()
{
    static LazyNeverDestroyed<StubBlob> blob;
    static std::once_flag once;
    std::call_once(once, [] {
        blob.construct();
        CCallHelpers jit;
#if CPU(ARM64)
        CCallHelpers::Label* labels = s_labels;
        Vector<std::pair<CCallHelpers::Call, Stub>> callsBetweenStubs;
        s_callsBetweenStubs = &callsBetweenStubs;
        Vector<CCallHelpers::Label> returnsIntoAdapters;
        s_returnsIntoAdapters = &returnsIntoAdapters;
        Vector<AddressOfLabel> addressesOfLabels;
        s_addressesOfLabels = &addressesOfLabels;
#else
        Vector<CCallHelpers::Label> returnsIntoAdapters;
        CCallHelpers::Label labels[numberOfStubs];
        Vector<std::pair<CCallHelpers::Call, Stub>> callsBetweenStubs;
#endif
#define AOT_GENERATE_STUB(name) \
        jit.align(); \
        labels[static_cast<unsigned>(Stub::name)] = jit.label(); \
        generate##name(jit);
        FOR_EACH_AOT_STUB(AOT_GENERATE_STUB)
#undef AOT_GENERATE_STUB

        Vector<CCallHelpers::Label> thunkLabels;
        if constexpr (usesStubs) {
            for (Stub stub : operationCallStubs) {
                for (unsigned entry = 0; entry < numberOfEntries; ++entry) {
                    thunkLabels.append(jit.label());
                    jit.move(CCallHelpers::TrustedImm32(entry * sizeof(void*)), GPRInfo::regT9);
                    jit.jump().linkTo(labels[static_cast<unsigned>(stub)], &jit);
                }
            }
            // These are short and extremely hot, so each entry point is followed by its own copy of the stub.
            for (Stub stub : functionCallStubs) {
                for (unsigned count = 0; count < numberOfCountsWithThunk; ++count) {
                    jit.align();
                    thunkLabels.append(jit.label());
                    jit.move(CCallHelpers::TrustedImm32(count), countGPR);
                    generateCallTo(jit, stub == Stub::Call ? CodeSpecializationKind::CodeForCall : CodeSpecializationKind::CodeForConstruct, count);
                }
            }
            for (unsigned frameSize = unitOfFrameSize; frameSize <= biggestFrameWithThunk; frameSize += unitOfFrameSize) {
                jit.align();
                thunkLabels.append(jit.label());
                jit.move(CCallHelpers::TrustedImm32(frameSize), GPRInfo::regT9);
                generatePrologue(jit);
            }
            for (unsigned i = 1; i <= numberOfStubIntrinsics; ++i) {
                StubIntrinsic intrinsic = static_cast<StubIntrinsic>(i);
                jit.align();
                thunkLabels.append(jit.label());
                CCallHelpers::JumpList otherwise;
                generateCallIntrinsic(jit, intrinsic, labels[static_cast<unsigned>(Stub::OperationVoidWithGlobalObject)], otherwise);
                otherwise.link(&jit);
                jit.move(T14, argumentGPR(0));
                jit.move(T15, argumentGPR(1));
                static_assert(functionCallStubs[0] == Stub::Call);
                jit.jump().linkTo(thunkLabels[firstThunkOfCalls + argumentCountOf(intrinsic)], &jit);
            }
#if CPU(ARM64)
            static_assert(!static_cast<unsigned>(ARM64Registers::x0));
            for (Stub stub : stubsWithAnyRegisterOperand) {
                for (unsigned number = 0; number < numberOfRegistersForOperand; ++number) {
                    GPRReg reg = static_cast<GPRReg>(number);
                    if (!operandMayBeIn(stub, reg) || reg == defaultOperandRegister(stub)) {
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
            CCallHelpers::Label handsBackIn[numberOfRegistersForResult];
            for (unsigned i = 0; i < numberOfRegistersForResult; ++i) {
                jit.align();
                handsBackIn[i] = jit.label();
                generateOperation(jit, Returns::Value, true, static_cast<GPRReg>(static_cast<unsigned>(ARM64Registers::x19) + i));
            }
            for (auto& [operation, acceptsOperandInAnyRegister] : operationsWithAnyRegisterResult) {
                for (unsigned i = 0; i < numberOfRegistersForResult; ++i) {
                    for (unsigned number = 0; number < numberOfRegistersForOperand; ++number) {
                        if (!acceptsOperandInAnyRegister && number) {
                            thunkLabels.append(thunkLabels.last());
                            continue;
                        }
                        thunkLabels.append(jit.label());
                        if (acceptsOperandInAnyRegister)
                            jit.move(static_cast<GPRReg>(number), GPRInfo::argumentGPR1);
                        jit.move(CCallHelpers::TrustedImm32(static_cast<unsigned>(operation) * sizeof(void*)), GPRInfo::regT9);
                        jit.jump().linkTo(handsBackIn[i], &jit);
                    }
                }
            }
#endif
            RELEASE_ASSERT(thunkLabels.size() == numberOfThunks);
        }

        // Linked in ordinary memory: the stubs only ever run from the copies in an image.
        jit.padBeforePatch();
        Vector<uint32_t> storage(jit.m_assembler.codeSize() / sizeof(uint32_t));
        LinkBuffer linkBuffer(jit, CodePtr<LinkBufferPtrTag>::fromUntaggedPtr(storage.mutableSpan().data()), storage.sizeInBytes(), LinkBuffer::Profile::Thunk);
        for (auto& [call, stub] : callsBetweenStubs)
            linkBuffer.link<JITThunkPtrTag>(call, linkBuffer.locationOf<JITThunkPtrTag>(labels[static_cast<unsigned>(stub)]));
#if CPU(ARM64)
        for (auto& address : addressesOfLabels) {
            auto* instruction = static_cast<uint8_t*>(linkBuffer.locationOf<JITThunkPtrTag>(address.instruction).untaggedPtr());
            int64_t delta = static_cast<uint8_t*>(linkBuffer.locationOf<JITThunkPtrTag>(address.target).untaggedPtr()) - instruction;
            RELEASE_ASSERT(delta >= -(1 << 20) && delta < (1 << 20));
            // adr reg, target
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
        // Branch compaction leaves nops at the end.
        while (size > sizeof(uint32_t) && *reinterpret_cast<const uint32_t*>(start + size - sizeof(uint32_t)) == 0xd503201f)
            size -= sizeof(uint32_t);
#endif
        blob->bytes.append(std::span { start, size });
        if (Options::verboseAOTCompilation()) [[unlikely]] {
            static constexpr ASCIILiteral names[] = {
#define AOT_STUB_NAME(name) #name ""_s,
                FOR_EACH_AOT_STUB(AOT_STUB_NAME)
#undef AOT_STUB_NAME
            };
            for (unsigned i = 0; i < numberOfStubs; ++i)
                dataLogLn("AOT: stub ", names[i], " ", blob->offsets[i]);
            // In the order in which they were generated above.
            unsigned thunk = 0;
            for (Stub stub : operationCallStubs) {
                dataLogLn("AOT: stub ThunksOf", names[static_cast<unsigned>(stub)], " ", blob->thunkOffsets[thunk]);
                thunk += numberOfEntries;
            }
            for (Stub stub : functionCallStubs) {
                for (unsigned count = 0; count < numberOfCountsWithThunk; ++count)
                    dataLogLn("AOT: stub ", names[static_cast<unsigned>(stub)], count, " ", blob->thunkOffsets[thunk++]);
            }
            for (unsigned frameSize = unitOfFrameSize; frameSize <= biggestFrameWithThunk; frameSize += unitOfFrameSize)
                dataLogLn("AOT: stub Prologue", frameSize, " ", blob->thunkOffsets[thunk++]);
            for (unsigned i = 1; i <= numberOfStubIntrinsics; ++i)
                dataLogLn("AOT: stub Intrinsic", i, " ", blob->thunkOffsets[thunk++]);
            for (Stub stub : stubsWithAnyRegisterOperand) {
                for (unsigned number = 0; number < numberOfRegistersForOperand; ++number, ++thunk) {
                    if (blob->thunkOffsets[thunk] != blob->offsets[static_cast<unsigned>(stub)])
                        dataLogLn("AOT: stub ", names[static_cast<unsigned>(stub)], "OfX", number, " ", blob->thunkOffsets[thunk]);
                }
            }
            for (auto& [stub, operation] : operationsWithAnyRegisterOperand) {
                for (unsigned number = 0; number < numberOfRegistersForOperand; ++number)
                    dataLogLn("AOT: stub ", names[static_cast<unsigned>(stub)], "#", static_cast<unsigned>(operation), "OfX", number, " ", blob->thunkOffsets[thunk++]);
            }
            for (Stub stub : stubsWithTwoAnyRegisterOperands) {
                for (unsigned i = 0; i < numberOfRegistersForPair * numberOfRegistersForPair; ++i)
                    dataLogLn("AOT: stub ", names[static_cast<unsigned>(stub)], "OfPair", i, " ", blob->thunkOffsets[thunk++]);
            }
            for (auto& [operation, acceptsOperandInAnyRegister] : operationsWithAnyRegisterResult) {
                for (unsigned i = 0; i < numberOfRegistersForResult * numberOfRegistersForOperand; ++i, ++thunk) {
                    if (acceptsOperandInAnyRegister || !(i % numberOfRegistersForOperand))
                        dataLogLn("AOT: stub OperationValueWithGlobalObject#", static_cast<unsigned>(operation), "ToX", 19 + i / numberOfRegistersForOperand, "OfX", i % numberOfRegistersForOperand, " ", blob->thunkOffsets[thunk]);
                }
            }
            dataLogLn("AOT: stub End ", size);
        }
    });
    return blob.get();
}

void StubCalls::call(CCallHelpers& jit, Stub stub, CallSite site)
{
    m_pending.append({ jit.nearCall(), stub, false, site.bits });
}

void StubCalls::call(CCallHelpers& jit, Stub stub, uint32_t valueOfT9, CallSite site)
{
    auto thunk = thunkFor(stub, valueOfT9);
    if (!thunk)
        jit.move(CCallHelpers::TrustedImm32(valueOfT9), GPRInfo::regT9);
    m_pending.append({ jit.nearCall(), stub, false, site.bits });
    if (thunk)
        m_pending.last().thunk = safeCast<uint16_t>(*thunk + 1);
}

void StubCalls::callWithOperandIn(CCallHelpers& jit, Stub stub, std::optional<uint32_t> valueOfT9, GPRReg operand, CallSite site)
{
    if (operand == defaultOperandRegister(stub)) {
        if (valueOfT9)
            call(jit, stub, *valueOfT9, site);
        else
            call(jit, stub, site);
        return;
    }
    m_pending.append({ jit.nearCall(), stub, false, site.bits });
    m_pending.last().thunk = safeCast<uint16_t>(thunkForOperandIn(stub, valueOfT9, operand) + 1);
}

void StubCalls::callWithOperandsIn(CCallHelpers& jit, Stub stub, GPRReg first, GPRReg second, CallSite site)
{
    if (second == GPRInfo::argumentGPR1 && (first == GPRInfo::argumentGPR0 || (acceptsOperandInAnyRegister(stub, std::nullopt) && operandMayBeIn(stub, first)))) {
        callWithOperandIn(jit, stub, std::nullopt, first, site);
        return;
    }
    auto numberOfFirst = numberAmongRegistersForPair(first);
    auto numberOfSecond = numberAmongRegistersForPair(second);
    if (!numberOfFirst || !numberOfSecond) {
        // The usual comparison is with 0, which lives in a pinned register: that one is moved and the other operand stays where it
        // is.
        if (first != GPRInfo::argumentGPR1 && acceptsOperandInAnyRegister(stub, std::nullopt) && operandMayBeIn(stub, first)) {
            jit.move(second, GPRInfo::argumentGPR1);
            callWithOperandIn(jit, stub, std::nullopt, first, site);
            return;
        }
        moveToFirstTwoArguments(jit, first, second);
        call(jit, stub, site);
        return;
    }
    for (unsigned i = 0; i < std::size(stubsWithTwoAnyRegisterOperands); ++i) {
        if (stubsWithTwoAnyRegisterOperands[i] != stub)
            continue;
        m_pending.append({ jit.nearCall(), stub, false, site.bits });
        m_pending.last().thunk = safeCast<uint16_t>(firstThunkOfPairs + (i * numberOfRegistersForPair + *numberOfFirst) * numberOfRegistersForPair + *numberOfSecond + 1);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void StubCalls::callForResultIn(CCallHelpers& jit, Stub stub, uint32_t valueOfT9, GPRReg operand, GPRReg result, CallSite site)
{
    if (result == GPRInfo::returnValueGPR) {
        if (operand == defaultOperandRegister(stub))
            call(jit, stub, valueOfT9, site);
        else
            callWithOperandIn(jit, stub, valueOfT9, operand, site);
        return;
    }
    unsigned which = *indexOfOperationWithAnyRegisterResult(stub, valueOfT9);
    unsigned numberOfResult = static_cast<unsigned>(result) - static_cast<unsigned>(ARM64Registers::x19);
    RELEASE_ASSERT(numberOfResult < numberOfRegistersForResult);
    RELEASE_ASSERT(operationsWithAnyRegisterResult[which].acceptsOperandInAnyRegister ? operandMayBeIn(stub, operand) : operand == defaultOperandRegister(stub));
    m_pending.append({ jit.nearCall(), stub, false, site.bits });
    m_pending.last().thunk = safeCast<uint16_t>(firstThunkOfResults + (which * numberOfRegistersForResult + numberOfResult) * numberOfRegistersForOperand + static_cast<unsigned>(operand) + 1);
}

void StubCalls::tailCall(CCallHelpers& jit, Stub stub)
{
    m_pending.append({ jit.nearTailCall(), stub, true, StubCall::noCallSite });
}

void StubCalls::tailCall(CCallHelpers& jit, Stub stub, uint32_t valueOfT9)
{
    auto thunk = thunkFor(stub, valueOfT9);
    if (!thunk)
        jit.move(CCallHelpers::TrustedImm32(valueOfT9), GPRInfo::regT9);
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

void IndexReferences::load(CCallHelpers& jit, GPRReg base, GPRReg dest, uint32_t addend, uint32_t scale)
{
    // If the scale is 4, the load is 32 bits wide.
    bool isHalfWord = scale == sizeof(uint32_t);
    RELEASE_ASSERT(isHalfWord ? !(addend % sizeof(uint32_t)) : !(addend % sizeof(void*)) && !(scale % sizeof(void*)));
    RELEASE_ASSERT(scale <= std::numeric_limits<uint16_t>::max());
    m_references.append({ jit.label(), addend, scale });
#if CPU(ARM64)
    jit.m_assembler.add<64>(dest, base, UInt12(0), 12);
    if (isHalfWord)
        jit.m_assembler.ldr<32>(dest, dest, 0u);
    else
        jit.m_assembler.ldr<64>(dest, dest, 0u);
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
    // add dest, base, #high, lsl #12; ldr dest, [dest, #low]. Both instructions have a 12-bit immediate at the same bit position,
    // and the second is scaled by the access size.
    uint64_t distance = reference.addend + static_cast<uint64_t>(index) * reference.scale;
    RELEASE_ASSERT(distance < (1u << 24));
    auto* instructions = reinterpret_cast<uint32_t*>(code + reference.offset);
    constexpr uint32_t immediate = 0xfffu << 10;
    RELEASE_ASSERT(!(instructions[0] & immediate) && !(instructions[1] & immediate));
    instructions[0] |= static_cast<uint32_t>(distance >> 12) << 10;
    instructions[1] |= static_cast<uint32_t>((distance & 0xfff) / (reference.scale == sizeof(uint32_t) ? sizeof(uint32_t) : sizeof(void*))) << 10;
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
        // A placeholder within range. ImageBuilder retargets every call when it places the code.
        linkBuffer.link<JITThunkPtrTag>(pending.call, CodeLocationLabel<JITThunkPtrTag>(tagCodePtr<JITThunkPtrTag>(start)));
        // The location of a near call is the end of the instruction; that of a near tail call is the instruction.
        auto* location = static_cast<uint8_t*>(linkBuffer.locationOfNearCall<JITThunkPtrTag>(pending.call).dataLocation());
        result.append({ static_cast<uint32_t>(location - start - (pending.isTailCall ? 0 : sizeof(uint32_t))), pending.stub, pending.isTailCall, pending.thunk, pending.function, pending.callSite });
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
        0x90000000u | (static_cast<uint32_t>(pages) & 3) << 29 | (static_cast<uint32_t>(pages >> 2) & 0x7ffff) << 5 | scratch, // adrp
        0x91000000u | static_cast<uint32_t>(target & 0xfff) << 10 | scratch << 5 | scratch, // add
        0xd61f0000u | scratch << 5, // br
    };
    static_assert(sizeof(instructions) == sizeOfVeneer);
    memcpy(base + veneer, instructions, sizeof(instructions));
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
#else
    UNUSED_PARAM(base);
    UNUSED_PARAM(instruction);
    UNUSED_PARAM(target);
    UNUSED_PARAM(isTailCall);
    RELEASE_ASSERT_NOT_REACHED();
#endif
}

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
