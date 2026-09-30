/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubs.h"

#if ENABLE(FTL_JIT)

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

// While the stubs are being generated: where those that have been are, and the calls from one to another, which are linked afterwards.
static CCallHelpers::Label s_labels[numberOfStubs];
static Vector<std::pair<CCallHelpers::Call, Stub>>* s_callsBetweenStubs;
static Vector<CCallHelpers::Label>* s_returnsIntoAdapters;
// Where the address of a place in the stubs is wanted in a register.
struct AddressOfLabel {
    CCallHelpers::Label instruction;
    GPRReg reg;
    CCallHelpers::Label target;
};
static Vector<AddressOfLabel>* s_addressesOfLabels;
static CCallHelpers::Label s_whereCallVarargsMakesTheCall;
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

// Leaves alone everything a function is passed.
static void generatePrologue(CCallHelpers& jit)
{
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfVM()), T11);
    jit.subPtr(GPRInfo::callFrameRegister, T9, T12);
    jit.loadPtr(Address(T11, VM::offsetOfSoftStackLimit()), T11);
    Jump overflow = jit.branchPtr(CCallHelpers::Above, T11, T12);
    // A frame so big that the subtraction wrapped around.
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

// Which function it is that has `pc` in it, in T15: what called a stub is told from where the stub is to go back to. Nothing is looked at
// but two tables of the image's, next to what they have for the code around it. Clobbers T14.
void loadIndexOfFunctionAt(CCallHelpers& jit, GPRReg pc)
{
    static_assert(indexOfFunctionGPR == T15);
    ASSERT(noOverlap(pc, T14, T15));
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfCode()), T14);
    jit.subPtr(pc, T14, T14);
    jit.urshiftPtr(T14, TrustedImm32(shiftOfGranuleOfCode), T15);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfGranulesOfCode()), CCallHelpers::memoryTempRegister);
    jit.load32(CCallHelpers::BaseIndex(CCallHelpers::memoryTempRegister, T15, CCallHelpers::TimesFour), T15);
    // That is the last to start no later than the granule does. It may be over by then.
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfStartsOfFunctionsAfterFirst()), CCallHelpers::memoryTempRegister);
    CCallHelpers::Label next = jit.label();
    jit.load32(CCallHelpers::BaseIndex(CCallHelpers::memoryTempRegister, T15, CCallHelpers::TimesFour), CCallHelpers::dataTempRegister);
    Jump found = jit.branch32(CCallHelpers::Above, CCallHelpers::dataTempRegister, T14);
    jit.add32(TrustedImm32(1), T15);
    jit.jump().linkTo(next, &jit);
    found.link(&jit);
}

// For a stub that has not called anything yet.
static void loadIndexOfCaller(CCallHelpers& jit)
{
    loadIndexOfFunctionAt(jit, CCallHelpers::linkRegister);
}

// The FunctionInfo of the function whose index is in T15, given its Instance.
static void loadInfo(CCallHelpers& jit, GPRReg instance, GPRReg result)
{
    ASSERT(result != instance && result != T15);
    static_assert(sizeof(FunctionInfo) == 32);
    jit.lshiftPtr(T15, TrustedImm32(5), result);
    jit.loadPtr(Address(instance, Instance::offsetOfInfos()), CCallHelpers::memoryTempRegister);
    jit.addPtr(CCallHelpers::memoryTempRegister, result);
}

// The Data that the slot is in, which is one of those of the function whose index is in T15: the function's own or, if it had none when
// it was called, the one that is nobody's (SharedData). Leaves the Instance in `instance`.
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

// After that. A slot has failed the function. If it is one of nobody's, that is counted, and a function it has happened to often
// enough (Instance::missesToPutUpWithFor()) is run often enough to have slots of its own: the next time it is called. Leaves all but
// T11 to T13 as they are. (This is for what may yet be found without an operation. Those count for themselves.)
static void countMissOfSlot(CCallHelpers& jit, GPRReg instance, GPRReg data)
{
    ASSERT(instance == T9 && data == T10);
    jit.loadPtr(Address(instance, Instance::offsetOfSharedData()), T11);
    Jump isItsOwn = jit.branchPtr(CCallHelpers::NotEqual, data, T11);
    jit.move(T15, T11);
    jit.addPtr(TrustedImm32(Instance::offsetOfStates()), instance, T12);
    jit.load32(CCallHelpers::BaseIndex(T12, T11, CCallHelpers::TimesFour), T13);
    // (It has been given one since it was called, and goes on without it until it returns. What is there is no longer a count.)
    Jump hasItsOwnByNow = jit.branch32(CCallHelpers::AboveOrEqual, T13, TrustedImm32(Instance::leastStateWithData));
    jit.add32(TrustedImm32(1), T13);
    // (The low half.)
    jit.store16(T13, CCallHelpers::BaseIndex(T12, T11, CCallHelpers::TimesFour));
    jit.zeroExtend16To32(T13, T13);
    static_assert(sizeof(FunctionInfo) == 32);
    jit.lshiftPtr(T11, TrustedImm32(5), T12);
    jit.loadPtr(Address(instance, Instance::offsetOfInfos()), CCallHelpers::memoryTempRegister);
    jit.addPtr(CCallHelpers::memoryTempRegister, T12);
    // Instance::missesToPutUpWithFor(). (What is too many to count is never reached, and such a function is not one that starts so.)
    jit.load16(Address(T12, FunctionInfo::offsetOfFlags()), T12);
    jit.urshift32(TrustedImm32(FunctionInfo::numberOfFlagBits), T12);
    jit.load32(Address(instance, Instance::offsetOfMissesForEightSlots()), CCallHelpers::memoryTempRegister);
    jit.mul32(CCallHelpers::memoryTempRegister, T12, T12);
    jit.urshift32(TrustedImm32(3), T12);
    jit.load32(Address(instance, Instance::offsetOfMissesToSpare()), CCallHelpers::memoryTempRegister);
    jit.add32(CCallHelpers::memoryTempRegister, T12);
    // (Once. Whoever is in the middle of it goes on counting.)
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

// The function is in `function`, its arguments are in place. The stub has a frame of its own for the while, as everything here does that
// calls what may look at the stack: what it is to go back to is where the calling function is at (see frameAt()).
static void callAndCheckException(CCallHelpers& jit, GPRReg function, Returns returns)
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
    jit.ret();
    exception.link(&jit);
    loadInstance(jit, T9);
    jumpToEntry(jit, T9, Entry::HandleException);
}

static void generateOperation(CCallHelpers& jit, Returns returns, bool withGlobalObject)
{
    loadInstance(jit, T11);
    if (withGlobalObject)
        jit.loadPtr(Address(T11, Instance::offsetOfGlobalObject()), A0);
    jit.loadPtr(Address(T11, Instance::offsetOfRuntimeTable()), T11);
    jit.loadPtr(CCallHelpers::BaseIndex(T11, T9, CCallHelpers::TimesOne), T11);
    callAndCheckException(jit, T11, returns);
}

static void generateColdOperation(CCallHelpers& jit, bool ofLeaf, bool returnsValue = false)
{
    constexpr GPRReg fp = GPRInfo::callFrameRegister;
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    constexpr GPRReg scratch = ARM64Registers::x16;
    if (ofLeaf) {
        // What its prologue would have done.
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

// Calls an operation that does not throw and does not look at the stack, and returns from the stub, with every register as it was
// (but for the return value register, if the result is wanted). For what is rare, so that the code that calls the stub need not
// have anything out of the way for it. setUp puts the arguments in place: it is given the Data in T10, which it leaves alone.
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

static void generateWriteBarrier(CCallHelpers& jit)
{
    jit.load8(Address(A0, JSCell::cellStateOffset()), T9);
    loadInstance(jit, T10);
    jit.loadPtr(Address(T10, Instance::offsetOfVM()), T11);
    jit.load32(Address(T11, VM::offsetOfHeapBarrierThreshold()), T11);
    Jump slow = jit.branch32(CCallHelpers::BelowOrEqual, T9, T11);
    jit.ret();

    slow.link(&jit);
    callPreservingRegistersAndReturn(jit, Entry::operationAOTWriteBarrier, false, [&] {
        jit.move(A0, A1);
        jit.loadPtr(Address(T10, Instance::offsetOfVM()), A0);
    });
}

static void generateToBoolean(CCallHelpers& jit)
{
    auto answer = [&](bool value) {
        jit.move(TrustedImm32(value), A0);
        jit.ret();
    };

    // false and true differ in the last bit.
    jit.xor64(TrustedImm32(JSValue::ValueFalse), A0, T9);
    Jump notBoolean = jit.branchTest64(CCallHelpers::NonZero, T9, TrustedImm32(~1));
    jit.move(T9, A0);
    jit.ret();

    notBoolean.link(&jit);
    jit.move(CCallHelpers::TrustedImm64(JSValue::NumberTag), T10);
    Jump notInt32 = jit.branch64(CCallHelpers::Below, A0, T10);
    jit.test32(CCallHelpers::NonZero, A0, A0, A0);
    jit.ret();

    notInt32.link(&jit);
    Jump notNumber = jit.branchTest64(CCallHelpers::Zero, A0, T10);
    // A double: false if it is a zero or not a number, which its bits say, less the sign.
    jit.add64(T10, A0, T9);
    jit.lshift64(T9, TrustedImm32(1), T9);
    Jump isZero = jit.branchTest64(CCallHelpers::Zero, T9);
    jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(0xffe0000000000000ULL)), T10);
    Jump isNaN = jit.branch64(CCallHelpers::Above, T9, T10);
    answer(true);
    isZero.link(&jit);
    isNaN.link(&jit);
    answer(false);

    notNumber.link(&jit);
    Jump isCell = jit.branchIfCell(A0, DoNotHaveTagRegisters);
    answer(false); // undefined, null.

    isCell.link(&jit);
    CCallHelpers::JumpList slow;
    jit.load8(Address(A0, JSCell::typeInfoTypeOffset()), T9);
    Jump notObject = jit.branch32(CCallHelpers::Below, T9, TrustedImm32(ObjectType));
    slow.append(jit.branchTest8(CCallHelpers::NonZero, Address(A0, JSCell::typeInfoFlagsOffset()), TrustedImm32(MasqueradesAsUndefined)));
    answer(true);

    notObject.link(&jit);
    slow.append(jit.branch32(CCallHelpers::NotEqual, T9, TrustedImm32(StringType)));
    jit.loadPtr(Address(A0, JSString::offsetOfValue()), T9);
    slow.append(jit.branchIfRopeStringImpl(T9));
    jit.load32(Address(T9, StringImpl::lengthMemoryOffset()), T9);
    jit.test32(CCallHelpers::NonZero, T9, T9, A0);
    jit.ret();

    slow.link(&jit);
    callPreservingRegistersAndReturn(jit, Entry::operationAOTToBoolean, true, [&] {
        jit.move(A0, A1);
        jit.loadPtr(Address(T10, Instance::offsetOfGlobalObject()), A0);
    });
}

static void generateEqual(CCallHelpers& jit, Entry operation)
{
    jit.move(CCallHelpers::TrustedImm64(JSValue::NumberTag), T9);
    Jump notBothInt32 = jit.branch64(CCallHelpers::Below, A0, T9);
    Jump notBothInt32Either = jit.branch64(CCallHelpers::Below, A1, T9);
    jit.compare32(CCallHelpers::Equal, A0, A1, A0);
    jit.ret();

    notBothInt32.link(&jit);
    notBothInt32Either.link(&jit);
    // The same bits, and not a number: equal, whatever it is.
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
static void generateLooseEqual(CCallHelpers& jit) { generateEqual(jit, Entry::operationAOTCompareEq); }

// operation(globalObject, A0, A1), for the call site in T10.
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

enum class Binary : uint8_t { Add, Sub, Mul, BitAnd, BitOr, BitXor, LShift, RShift, URShift, Less, LessEq, Greater, GreaterEq };

// A function that has no loop in it may well be called from one, so numbers are dealt with here. The rest is the runtime's.
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
        slow.append(jit.branchTest32(CCallHelpers::Zero, T11)); // It may be -0.
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

// A0 is a cell and the int32 in A1 is not the index of anything in a butterfly. If A0 is a typed array of fixed length that has such
// an element, leaves the type of the array in T13, its storage in T11 and the index, zero extended, in T12.
static void checkTypedArrayAccess(CCallHelpers& jit, CCallHelpers::JumpList& slow)
{
    jit.load8(Address(A0, JSCell::typeInfoTypeOffset()), T13);
    jit.sub32(T13, TrustedImm32(FirstTypedArrayType), T11);
    slow.append(jit.branch32(CCallHelpers::AboveOrEqual, T11, TrustedImm32(NumberOfTypedArrayTypesExcludingDataView)));
    slow.append(jit.branchTest8(CCallHelpers::NonZero, Address(A0, JSArrayBufferView::offsetOfMode()), TrustedImm32(isResizableOrGrowableSharedMode)));
    jit.zeroExtend32ToWord(A1, T12);
    // One that has been detached has no length.
    jit.loadPtr(Address(A0, JSArrayBufferView::offsetOfLength()), T11);
    slow.append(jit.branchPtr(CCallHelpers::AboveOrEqual, T12, T11));
    jit.loadPtr(Address(A0, JSArrayBufferView::offsetOfVector()), T11);
}

static void getFromMegamorphicCache(CCallHelpers&, GPRReg uid, CCallHelpers::JumpList& notFound);

static void generateGetByVal(CCallHelpers& jit)
{
    constexpr FPRReg number = FPRInfo::fpRegT0;
    CCallHelpers::JumpList slow;
    jit.move(CCallHelpers::TrustedImm64(JSValue::NumberTag), T9);
    Jump notInt32 = jit.branch64(CCallHelpers::Below, A1, T9);
    CCallHelpers::Label haveIndex = jit.label();
    slow.append(jit.branchIfNotCell(A0, DoNotHaveTagRegisters));
    jit.load8(Address(A0, JSCell::indexingTypeAndMiscOffset()), T13);
    jit.and32(TrustedImm32(IndexingShapeMask), T13);
    Jump noButterfly = jit.branchTest32(CCallHelpers::Zero, T13);
    jit.loadPtr(Address(A0, JSObject::butterflyOffset()), T11);
    jit.zeroExtend32ToWord(A1, T12);
    Jump holdsDoubles = jit.branch32(CCallHelpers::Equal, T13, TrustedImm32(DoubleShape));
    slow.append(jit.branch32(CCallHelpers::Above, T13, TrustedImm32(ContiguousShape)));
    static_assert(Int32Shape < DoubleShape && DoubleShape < ContiguousShape);
    slow.append(jit.branch32(CCallHelpers::Below, T13, TrustedImm32(Int32Shape)));

    // An element that is there, in storage that holds JSValues.
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

    // A hole is not a number.
    holdsDoubles.link(&jit);
    slow.append(jit.branch32(CCallHelpers::AboveOrEqual, T12, Address(T11, Butterfly::offsetOfPublicLength())));
    jit.loadDouble(CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesEight), number);
    slow.append(jit.branchIfNaN(number));
    boxDoubleAndReturn();

    noButterfly.link(&jit);
    // A character of a string, of the kind that there is one string of for the whole VM.
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

    // A double that is an integer names the same property as the integer. (So does minus zero.)
    notInt32.link(&jit);
    Jump notNumber = jit.branchTest64(CCallHelpers::Zero, A1, T9);
    jit.add64(T9, A1, T11);
    jit.move64ToDouble(T11, number);
    jit.branchConvertDoubleToInt32(number, T11, slow, FPRInfo::fpRegT1, false);
    jit.or64(T9, T11, A1);
    jit.jump().linkTo(haveIndex, &jit);

    // A name: a string that is an atom, or a symbol.
    notNumber.link(&jit);
    slow.append(jit.branchIfNotCell(A0, DoNotHaveTagRegisters));
    slow.append(jit.branchIfNotObject(A0));
    slow.append(jit.branchIfNotCell(A1, DoNotHaveTagRegisters));
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

static void generatePutByVal(CCallHelpers& jit)
{
    // An element for which there is room in contiguous storage that is the object's own to write to.
    CCallHelpers::JumpList slow;
    jit.move(CCallHelpers::TrustedImm64(JSValue::NumberTag), T9);
    slow.append(jit.branch64(CCallHelpers::Below, A1, T9));
    slow.append(jit.branchIfNotCell(A0, DoNotHaveTagRegisters));
    jit.load8(Address(A0, JSCell::indexingTypeAndMiscOffset()), T11);
    jit.and32(TrustedImm32(IndexingShapeMask | CopyOnWrite), T11);
    Jump isContiguous = jit.branch32(CCallHelpers::Equal, T11, TrustedImm32(ContiguousShape));
    slow.append(jit.branchTest32(CCallHelpers::NonZero, T11));

    // A typed array, and a number that needs no more than to be cut to size.
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
    // What is between the old length and here are holes already.
    jit.add32(TrustedImm32(1), T12, T13);
    jit.store32(T13, Address(T11, Butterfly::offsetOfPublicLength()));
    inBounds.link(&jit);
    jit.store64(A2, CCallHelpers::BaseIndex(T11, T12, CCallHelpers::TimesEight));

    Jump notCell = jit.branchIfNotCell(A2, DoNotHaveTagRegisters);
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

// ---- Sites

// A site is passed as the address of its slot: the function has its Data at hand, and this way what is looked at first is one load
// away. Which site it is only matters when the slot is of no help.
static Address slotWord(GPRReg slot, unsigned word) { return Address(slot, word * sizeof(void*)); }

// data: the Data. slot: the address of one of its slots. Leaves the index of that in result.
static void siteOfSlot(CCallHelpers& jit, GPRReg data, GPRReg slot, GPRReg result)
{
    static_assert(sizeof(Slot) == 16);
    jit.subPtr(slot, data, result);
    jit.subPtr(TrustedImm32(Data::offsetOfSlots()), result);
    jit.urshiftPtr(TrustedImm32(4), result);
}

// The slot did not have it. The operands are in A0.., the site follows them. Calls
//     operation(globalObject, operands..., identifier, Slot*, extra)
// This much puts the arguments in place and leaves the operation in T9.
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

// word: the first word of a slot whose offset is locationOfProperty(). object: where the property is. Leaves the storage in
// `storage` and the index into it in `word`.
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

// A0 = a cell. slot: that of a property access. If the cell is of a shape that the compiler knew of (Structure::knownShape()), what the
// program's dispatch table has for that and the site's selector: where the property is, in words, from the start of the object or, if
// negative, from its butterfly, in T12. T9: the Instance. T10: the Data the slot is in. T15: which function. Leaves those, and the
// selector where it is told to. Clobbers T11, T13.
static void findInDispatchTable(CCallHelpers& jit, GPRReg slot, GPRReg selector, CCallHelpers::JumpList& notOfKnownShape, CCallHelpers::JumpList& notOwn)
{
    jit.load32(Address(A0, JSCell::structureIDOffset()), T11);
    jit.loadPtr(Address(T9, Instance::offsetOfStructureIDBase()), T12);
    jit.addPtr(T12, T11);
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

// After that, for a property that is in the object itself. The site's slot may as well have it, if it has nothing: code that is in a
// hurry looks there without coming to a stub. Leaves A0 to A2, T9, T10 and T12 as they are.
static void fillEmptySlotFromDispatchTable(CCallHelpers& jit, GPRReg slot)
{
    constexpr GPRReg word = A4;
    constexpr GPRReg scratch = A5;
    jit.loadPtr(Address(T9, Instance::offsetOfSharedData()), scratch);
    Jump slotIsNobodys = jit.branchPtr(CCallHelpers::Equal, T10, scratch);
    jit.load64(slotWord(slot, 0), word);
    Jump slotIsTaken = jit.branchTest32(CCallHelpers::NonZero, word);
    Jump slotHasMore = jit.branchTestPtr(CCallHelpers::NonZero, slotWord(slot, 1));
    // The collector wants to know which functions to look at.
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

// A0 = the base, T12 = a GetterSetter. If its getter is a function it is called from here, and returns to whoever called the stub.
// Leaves A0 and A1 alone if not.
static void callGetter(CCallHelpers& jit, CCallHelpers::JumpList& cannot)
{
    jit.loadPtr(Address(T12, GetterSetter::offsetOfGetter()), T12);
    cannot.append(jit.branchIfNotType(T12, JSFunctionType));
    jit.move(A0, thisGPR);
    jit.move(T12, calleeGPR);
    jit.move(TrustedImm32(0), countGPR);
    jit.jump().linkTo(s_labels[static_cast<unsigned>(Stub::Call)], &jit);
}

// A0 = the base, an object. uid: the name, an atom or the name of a symbol. What the VM's megamorphic cache has for that is what the
// stub returns; if it is a getter, what that returns (see callGetter()). Leaves A0, A1, T10 and uid alone if it has nothing.
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
static void generateGetById(CCallHelpers& jit) { generateGetByIdWith(jit, Entry::operationAOTGetById); }
static void generateGetByIdWellKnown(CCallHelpers& jit) { generateGetByIdWith(jit, Entry::operationAOTGetByIdWellKnown); }

static void generateGetByIdWith(CCallHelpers& jit, Entry operation)
{
    CCallHelpers::JumpList miss;
    miss.append(jit.branchIfNotCell(A0, DoNotHaveTagRegisters));
    jit.load64(slotWord(A1, 0), T11);
    jit.load32(Address(A0, JSCell::structureIDOffset()), T12);
    miss.append(jit.branch32(CCallHelpers::NotEqual, T11, T12));
    // In the base, or in an object that every base of this structure inherits it from.
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

    miss.link(&jit);
    if (operation == Entry::operationAOTGetById) {
        loadIndexOfCaller(jit);
        // An object of a shape that the compiler knew of: where its properties are is in the program's dispatch table.
        CCallHelpers::JumpList notInTable;
        CCallHelpers::JumpList notOwn;
        loadInstanceAndDataOfSlot(jit, A1, T9, T10);
        countMissOfSlot(jit, T9, T10);
        notInTable.append(jit.branchIfNotCell(A0, DoNotHaveTagRegisters));
        findInDispatchTable(jit, A1, A2, notInTable, notOwn);
        Jump isOutOfLine = jit.branch64(CCallHelpers::LessThan, T12, TrustedImm32(0));
        fillEmptySlotFromDispatchTable(jit, A1);
        jit.load64(CCallHelpers::BaseIndex(A0, T12, CCallHelpers::TimesEight), A0);
        jit.ret();

        isOutOfLine.link(&jit);
        jit.loadPtr(Address(A0, JSObject::butterflyOffset()), T13);
        jit.load64(CCallHelpers::BaseIndex(T13, T12, CCallHelpers::TimesEight), A0);
        jit.ret();

        // It has no such property of its own. See Instance::lookAtObjectPrototype().
        notOwn.link(&jit);
        jit.load32(Address(A0, JSCell::structureIDOffset()), T12);
        jit.loadPtr(Address(T9, Instance::offsetOfStructureIDBase()), T13);
        jit.addPtr(T13, T12);
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
        notFound.append(jit.branchIfNotCell(A0, DoNotHaveTagRegisters));
        notFound.append(jit.branchIfNotObject(A0));
        jit.load32(Address(A1, OBJECT_OFFSETOF(Slot, offset)), T11);
        jit.and32(TrustedImm32(Slot::attemptsMask), T11);
        if (!Options::aotLooksInMegamorphicCacheUnlessSlotIsEmpty()) {
            notFound.append(jit.branch32(CCallHelpers::NotEqual, T11, TrustedImm32(Slot::attemptsMask)));
            loadInstanceAndDataOfSlot(jit, A1, T9, T10);
        } else {
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
        }
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
}

static void generatePutById(CCallHelpers& jit)
{
    CCallHelpers::JumpList miss;
    miss.append(jit.branchIfNotCell(A0, DoNotHaveTagRegisters));
    jit.load64(slotWord(A2, 0), T11);
    jit.load32(Address(A0, JSCell::structureIDOffset()), T12);
    miss.append(jit.branch32(CCallHelpers::NotEqual, T11, T12));
    locateCachedProperty(jit, A0, T11, T12);
    jit.store64(A1, CCallHelpers::BaseIndex(T12, T11, CCallHelpers::TimesEight));
    jit.load32(slotWord(A2, 1), T11);
    Jump sameStructure = jit.branchTest32(CCallHelpers::Zero, T11);
    jit.store32(T11, Address(A0, JSCell::structureIDOffset()));
    sameStructure.link(&jit);

    CCallHelpers::Label stored = jit.label();
    Jump notCell = jit.branchIfNotCell(A1, DoNotHaveTagRegisters);
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

    // A property that an object of a shape the compiler knew of has, all of which are plain ones that can be stored to.
    miss.link(&jit);
    CCallHelpers::JumpList notInTable;
    loadIndexOfCaller(jit);
    loadInstanceAndDataOfSlot(jit, A2, T9, T10);
    countMissOfSlot(jit, T9, T10);
    // (Not where a slot says what it holds: the table does not say which do. See SlotsOfBornObjects.)
    if (!Options::aotTypesFields()) {
        notInTable.append(jit.branchIfNotCell(A0, DoNotHaveTagRegisters));
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
    missAtSite(jit, Entry::operationAOTPutById, 2, Returns::Void);
}

// The caches for private names are for a structure and a name, which is a cell that the slot points to.
static void checkPrivateNameCache(CCallHelpers& jit, GPRReg slot, CCallHelpers::JumpList& miss)
{
    miss.append(jit.branchIfNotCell(A0, DoNotHaveTagRegisters));
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
    // Only for a field that is there already.
    CCallHelpers::JumpList miss;
    checkPrivateNameCache(jit, A3, miss);
    locateCachedProperty(jit, A0, T11, T12);
    jit.store64(A2, CCallHelpers::BaseIndex(T12, T11, CCallHelpers::TimesEight));

    Jump notCell = jit.branchIfNotCell(A2, DoNotHaveTagRegisters);
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

// T11: the offset of an op_resolve_scope's slot. A0: the scope to start from. If the slot says how far out the scope is, leaves that
// scope in A0. Else takes the jump.
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

// See operationAOTGetFromScope(). A0 = the scope. The slot's words are firstWord and the next of those at A1. Returns from the stub
// with the value if the slot has it.
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
    // See cacheVariableOfEnvironment(), which is the only thing there is a cache for.
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

    Jump notCell = jit.branchIfNotCell(A1, DoNotHaveTagRegisters);
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
    generic.append(jit.branchIfNotCell(A0, DoNotHaveTagRegisters));

    // An array that has storage of some kind has its length there. One above what an int32 holds is for the runtime.
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

// vm: the VM. lookUp: operationLookupExceptionHandler(). Both in registers that a call does not preserve. The frame pointer and the
// link register say where whoever noticed is at. Does not come back.
static void unwind(CCallHelpers& jit, GPRReg vm, GPRReg lookUp)
{
    constexpr GPRReg keptVM = GPRInfo::regCS0;
    RELEASE_ASSERT(noOverlap(vm, lookUp, A0, GPRInfo::regT1));
    jit.emitFunctionPrologue();
    jit.move(vm, GPRInfo::regT1);
    jit.copyCalleeSavesToVMEntryFrameCalleeSavesBuffer(GPRInfo::regT1);
    // Which leaves them free to use: the handler gets them from there.
    jit.move(vm, keptVM);
    jit.move(vm, A0);
    jit.call(lookUp, OperationPtrTag);
    // genericUnwind() leaves the handler's frame in VM::callFrameForCatch, and where to go in VM::targetMachinePCForThrow. Whatever
    // is there, of whoever's making, begins by finding the VM from the frame it is entered in, which is still that of whoever
    // noticed; and a frame of this compiler's code says nothing. So it is given something that says as much as a frame of
    // WebAssembly's does: an instance where a CodeBlock would be, that has the VM in it.
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

// In the frame of whoever there is no room to do something for, which is where it is thrown.
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
    // The function has saved no register yet, and its frame is nobody's. It is the caller that fails to call it.
    jit.emitFunctionEpilogue();
    throwStackOverflow(jit);
}

static void generateCatch(CCallHelpers& jit)
{
    // The frame pointer is still that of whatever threw, or noticed. Like the interpreter's handlers, this finds the VM from its callee.
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

// ---- Coming in from outside

// Where whoever calls a function the way the rest of the engine calls one ends up: the interpreter, C++, a stub that was given something
// to call that it could make nothing of. A frame of that kind is being made, and nothing has been pushed. It becomes the frame of the
// stub, which is what stands between the two conventions for as long as the function runs. word: the function's EntryWord.
static void adapt(CCallHelpers& jit, GPRReg word, GPRReg instance)
{
    ASSERT(noOverlap(word, instance, thisGPR, countGPR, calleeGPR, T13));
    jit.emitFunctionPrologue();
    static_assert(!(sizeOfWhatAdapterSaves % stackAlignmentBytes()));
    jit.subPtr(TrustedImm32(sizeOfWhatAdapterSaves), CCallHelpers::stackPointerRegister);
    jit.storePtr(instanceGPR, Address(GPRInfo::callFrameRegister, offsetOfInstanceRegisterInAdapter));
    jit.storePtr(GPRInfo::numberTagRegister, Address(GPRInfo::callFrameRegister, offsetOfNumberTagRegisterInAdapter));
    jit.storePtr(GPRInfo::notCellMaskRegister, Address(GPRInfo::callFrameRegister, offsetOfNotCellMaskRegisterInAdapter));
    jit.storePtr(instance, Address(GPRInfo::callFrameRegister, offsetOfInstanceInAdapter));
    jit.move(instance, instanceGPR);
    jit.emitMaterializeTagCheckRegisters();
    // Nobody is to take it for a frame of the interpreter's.
    jit.storePtr(CCallHelpers::TrustedImmPtr(nullptr), CCallHelpers::addressFor(CallFrameSlot::codeBlock));

    jit.load64(CCallHelpers::addressFor(CallFrameSlot::callee), calleeGPR);
    jit.load64(CCallHelpers::addressFor(CallFrameSlot::thisArgument), thisGPR);
    jit.load32(CCallHelpers::lowWordFor(CallFrameSlot::argumentCountIncludingThis), countGPR);
    jit.sub32(TrustedImm32(1), countGPR);
    Jump takesList = jit.branchTest64(CCallHelpers::NonZero, word, CCallHelpers::TrustedImm64(1LL << EntryWord::bitOfIsList));
    // (What is above the last argument is somebody's, and there: it is looked at and not used.)
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

// The code of a program or a module, which is entered with a CodeBlock, as a frame of the interpreter's would be.
static void generateEnter(CCallHelpers& jit)
{
    jit.loadPtr(slotOfFrameBeingMade(CallFrameSlot::codeBlock), T11);
    jit.loadPtr(Address(T11, CodeBlock::offsetOfGlobalObject()), T12);
    jit.loadPtr(Address(T11, CodeBlock::jitCodeOffset()), T11);
    jit.loadPtr(Address(T12, JSGlobalObject::offsetOfAOTInstance()), T12);
    jit.loadPtr(Address(T11, JITCode::offsetOfEntry()), T11);
    adapt(jit, T11, T12);
}

// A function, which has no CodeBlock. What its callers put in the frame for one is nothing.
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

// The frame is made, but for its CodeBlock. T0 = callee, T2 = the CallLinkInfo of a VirtualCallInfo, if it comes to that.
template<typename LoadCallLinkInfo>
static void dispatchCall(CCallHelpers& jit, CodeSpecializationKind kind, const LoadCallLinkInfo& loadCallLinkInfo)
{
    auto slotOfNewFrame = [](CallFrameSlot slot, ptrdiff_t offset = 0) {
        return Address(CCallHelpers::stackPointerRegister, (static_cast<int>(slot) - CallerFrameAndPC::sizeInRegisters) * static_cast<int>(sizeof(Register)) + offset);
    };

    // A function that has code, which is what nearly every callee is.
    CCallHelpers::JumpList slow;
    slow.append(jit.branchIfNotCell(BaselineJITRegisters::Call::calleeGPR, DoNotHaveTagRegisters));
    slow.append(jit.branchIfNotFunction(BaselineJITRegisters::Call::calleeGPR));
    jit.loadPtr(Address(BaselineJITRegisters::Call::calleeGPR, JSFunction::offsetOfExecutableOrRareData()), T11);
    Jump hasExecutable = jit.branchTestPtr(CCallHelpers::Zero, T11, TrustedImm32(JSFunction::rareDataTag));
    jit.loadPtr(Address(T11, FunctionRareData::offsetOfExecutable() - JSFunction::rareDataTag), T11);
    hasExecutable.link(&jit);
    jit.loadPtr(Address(T11, ExecutableBase::offsetOfJITCodeWithArityCheckFor(kind)), T12);
    slow.append(jit.branchTestPtr(CCallHelpers::Zero, T12));
    Jump isNative = jit.branchIfNotType(T11, FunctionExecutableType);
    jit.loadPtr(Address(T11, FunctionExecutable::offsetOfCodeBlockFor(kind)), T11);
    jit.storePtr(T11, slotOfNewFrame(CallFrameSlot::codeBlock));
    isNative.link(&jit);
    jit.farJump(T12, JSEntryPtrTag);

    // Anything else is for llint_virtual_call() to find out about, in the frame of the callee. This may be a tail call, whose
    // caller is gone: everything there is to know comes with the CallLinkInfo.
    slow.link(&jit);
    loadCallLinkInfo();
    findTargetAndCall(jit);
}

static void findTargetAndCall(CCallHelpers& jit)
{
    constexpr GPRReg info = BaselineJITRegisters::Call::callLinkInfoGPR;
    jit.emitFunctionPrologue();
    // As emitCallSlowPath(): the operation's frame goes where the last callee at this depth had its own.
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
    // The function to call or the like of it. Or, with the VM, nothing: it threw.
    Jump threw = jit.branchTestPtr(CCallHelpers::NonZero, GPRInfo::returnValueGPR2);
    jit.farJump(GPRInfo::returnValueGPR, JSEntryPtrTag);

    threw.link(&jit);
    jit.move(GPRInfo::returnValueGPR2, T10);
    jit.loadPtr(Address(T11, VirtualCallInfo::offsetOfLookupExceptionHandler()), T11);
    unwind(jit, T10, T11);
}

// Where a FunctionExecutable that was made when the program was built says its code is, to whoever makes frames the way the
// interpreter wants them. Nothing is written in such an executable: which function it is, and where the code for that is, it says;
// the rest is found from the callee. The first time, the function has nothing of the realm yet, and findCallTarget() sees to that.
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

// What the NativeExecutable of bound functions whose target is a function has for code to be called with, where there is no JIT: what
// boundFunctionCallGenerator() makes (ThunkGenerators.cpp), but that it goes by no address. The frame is that of a native
// function, as far as anybody who walks the stack can tell. If there is no room for the target's, or the target has no code yet, it is as
// if this had not been here: the executable's function sees to it.
static void generateCallBoundFunction(CCallHelpers& jit)
{
    constexpr GPRReg bound = GPRInfo::regT0;
    constexpr GPRReg total = GPRInfo::regT1;
    constexpr GPRReg scratch = GPRInfo::regT2;
    constexpr GPRReg passed = GPRInfo::regT3;
    constexpr GPRReg value = GPRInfo::regT4;
    constexpr GPRReg vm = T10;
    CCallHelpers::JumpList theLongWay;

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
    theLongWay.append(jit.branchPtr(CCallHelpers::Above, T11, scratch));
    jit.move(scratch, CCallHelpers::stackPointerRegister);

    jit.store32(total, CCallHelpers::calleeFrameLowWordSlot(CallFrameSlot::argumentCountIncludingThis));
    jit.loadValue(Address(bound, JSBoundFunction::offsetOfBoundThis()), value);
    jit.storeValue(value, CCallHelpers::calleeArgumentSlot(0));

    // What was passed comes last.
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
    jit.loadPtr(Address(total, ExecutableBase::offsetOfJITCodeWithArityCheckFor(CodeSpecializationKind::CodeForCall)), scratch);
    theLongWay.append(jit.branchTestPtr(CCallHelpers::Zero, scratch));
    Jump isNative = jit.branchIfNotType(total, FunctionExecutableType);
    jit.loadPtr(Address(total, FunctionExecutable::offsetOfCodeBlockForCall()), passed);
    jit.storePtr(passed, CCallHelpers::calleeFrameCodeBlockBeforeCall());
    isNative.link(&jit);
    jit.call(scratch, JSEntryPtrTag);
    jit.emitFunctionEpilogue();
    jit.ret();

    theLongWay.link(&jit);
    jit.emitFunctionEpilogue();
    jit.loadPtr(Address(vm, VM::offsetOfAOTRuntimeTable()), T11);
    jit.loadPtr(Address(T11, static_cast<unsigned>(Entry::NativeCallTrampoline) * sizeof(void*)), T11);
    jit.farJump(T11, JSEntryPtrTag);
}

// What a FunctionExecutable that was made when the program was built has for code to construct with, if its function was compiled
// to be called and that is all (FunctionExecutable::constructsByCalling()). Few such functions ever see a `new`. The frame is that
// of a native function, as far as anybody who walks the stack can tell.
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
    // What was constructed. Or, with the VM, nothing: it threw.
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

// calleeGPR: what is to be called. If it is a function that has code of this compiler's, that has been run in this realm before, leaves
// its EntryWord in T12. Clobbers T11, T13.
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
    otherwise.append(jit.branchTest64(CCallHelpers::Zero, T12));
    jit.load32(Address(T11, FunctionExecutable::offsetOfAOTIndexFor(kind)), T13);
    if (kind == CodeSpecializationKind::CodeForConstruct)
        otherwise.append(jit.branch32(CCallHelpers::Equal, T13, TrustedImm32(static_cast<int32_t>(FunctionExecutable::aotIndexOfWhatConstructsByCalling))));
    jit.addPtr(TrustedImm32(Instance::offsetOfStates()), instanceGPR, T11);
    jit.load32(CCallHelpers::BaseIndex(T11, T13, CCallHelpers::TimesFour), T11);
    otherwise.append(jit.branch32(CCallHelpers::Below, T11, TrustedImm32(Instance::isLinkedWithoutData)));
}

// In a frame of the stub's, with a frame of the kind the rest of the engine makes below it, complete but for its CodeBlock: calls
// whatever calleeGPR is the way the rest of the engine would, and returns what that returns to whoever called the stub.
static void callTheWayTheEngineDoes(CCallHelpers& jit, CodeSpecializationKind kind)
{
    jit.move(calleeGPR, BaselineJITRegisters::Call::calleeGPR);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfRuntimeTable()), T11);
    jit.loadPtr(Address(T11, static_cast<unsigned>(kind == CodeSpecializationKind::CodeForCall ? Entry::CallLinkInfoForCall : Entry::CallLinkInfoForConstruct) * sizeof(void*)), BaselineJITRegisters::Call::callLinkInfoGPR);
    callStubFromStub(jit, kind == CodeSpecializationKind::CodeForCall ? Stub::VirtualCall : Stub::VirtualConstruct);
    jit.emitFunctionEpilogue();
    jit.ret();
}

// See Stub::Call. count: what is in countGPR, if that is known here.
static void generateCallTo(CCallHelpers& jit, CodeSpecializationKind kind, std::optional<unsigned> count)
{
    CCallHelpers::JumpList theLongWay;
    findCodeOfCallee(jit, kind, theLongWay);
    Jump takesList = jit.branchTest64(CCallHelpers::NonZero, T12, CCallHelpers::TrustedImm64(1LL << EntryWord::bitOfIsList));
    jit.urshift64(T12, TrustedImm32(EntryWord::shiftOfNumberOfParameters), T13);
    jit.and64(CCallHelpers::TrustedImm64(static_cast<int64_t>(EntryWord::addressMask)), T12);
    Jump enough = count ? jit.branch32(CCallHelpers::BelowOrEqual, T13, TrustedImm32(*count)) : jit.branch32(CCallHelpers::BelowOrEqual, T13, countGPR);
    // (More of them than the function has parameters is no harm.)
    jit.move(CCallHelpers::TrustedImm64(JSValue::ValueUndefined), T13);
    for (unsigned i = count.value_or(0); i < numberOfArgumentGPRs; ++i) {
        if (count)
            jit.move(T13, argumentGPR(i));
        else
            jit.moveConditionally32(CCallHelpers::Above, countGPR, TrustedImm32(i), argumentGPR(i), T13, argumentGPR(i));
    }
    enough.link(&jit);
    jit.farJump(T12, JSEntryPtrTag);

    // What was passed goes where the rest of the engine, and a function that wants a list, would have it.
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

    theLongWay.link(&jit);
    spill();
    callTheWayTheEngineDoes(jit, kind);
}

// See Stub::CallList.
static void generateCallListTo(CCallHelpers& jit, CodeSpecializationKind kind)
{
    CCallHelpers::JumpList theLongWay;
    findCodeOfCallee(jit, kind, theLongWay);
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

    theLongWay.link(&jit);
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

    // (globalObject, list, first): how many there are.
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

    // (globalObject, where to, list, first, how many)
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
        // The list goes to where the frame of the function is that called this, right below what that begins with: which is what a frame
        // of this stub's would begin with there. If that function was itself called from such a frame, that one goes too.
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
        // (Upwards, so the last first.)
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
        jit.jump().linkTo(s_whereCallVarargsMakesTheCall, &jit);
    } else {
        if (kind == CodeSpecializationKind::CodeForCall)
            s_whereCallVarargsMakesTheCall = jit.label();
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

static void generateIteratorNext(CCallHelpers& jit)
{
    CCallHelpers::JumpList generic;
    CCallHelpers::JumpList indexSlow;
    auto handled = [&] {
        jit.move(TrustedImm32(0), T9);
        jit.ret();
    };
    // value: what an operation behind a shortcut handed back, which is nothing at the end. Leaves done in A0.
    auto doneIfEmpty = [&](GPRReg value) {
        static_assert(JSValue::ValueTrue == JSValue::ValueFalse + 1);
        jit.compare64(CCallHelpers::Equal, value, TrustedImm32(0), A0);
        jit.add64(TrustedImm32(JSValue::ValueFalse), A0);
    };
    // Calls the operation with room on the stack for a word that outlives the call, which starts out as `kept`.
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

    Jump nextIsCell = jit.branchIfCell(A0, DoNotHaveTagRegisters);

    // Then, and only then, iterator may be a sentinel instead of an object: iterable is an array, and next the index to visit.
    generic.append(jit.branchIfNotCell(A1, DoNotHaveTagRegisters));
    generic.append(jit.branchIfNotType(A1, SentinelType));

    // An element that is there, in storage that holds JSValues. The end, holes and everything else are the runtime's.
    jit.move(CCallHelpers::TrustedImm64(JSValue::NumberTag), T9);
    indexSlow.append(jit.branch64(CCallHelpers::Below, A0, T9));
    indexSlow.append(jit.branchIfNotCell(A2, DoNotHaveTagRegisters));
    indexSlow.append(jit.branchIfNotType(A2, ArrayType));
    jit.load8(Address(A2, JSCell::indexingTypeAndMiscOffset()), T11);
    jit.and32(TrustedImm32(IndexingShapeMask), T11);
    Jump isInt32Shape = jit.branch32(CCallHelpers::Equal, T11, TrustedImm32(Int32Shape));
    indexSlow.append(jit.branch32(CCallHelpers::NotEqual, T11, TrustedImm32(ContiguousShape)));
    isInt32Shape.link(&jit);
    jit.loadPtr(Address(A2, JSObject::butterflyOffset()), T11);
    jit.load32(Address(T11, Butterfly::offsetOfPublicLength()), T12);
    // As unsigned: the index of a finished iteration, -1, is above any length.
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

    generic.link(&jit);
    jit.move(TrustedImm32(1), T9);
    jit.ret();
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

// What follows was written for A0 = the callee, with `this` and the arguments in memory, and for leaving all of that as it was on every way
// out to `otherwise`. So that is how it is entered: the arguments, of which there are at most two, are put aside in T14 and T15, and are
// put back on the way out (generateCallIntrinsic()). It has no use for thisGPR and calleeGPR but to read them.
// What comes back from here is what the function would have returned.
// A1 = a Map or a Set, A2 = a key. Goes to `otherwise` if the key, or one that is in the way, is more than this can make sense of;
// until then A0, A1, A2, A3 and T10 are as they were. Leaves where the key is, among the entries, in A6, or goes to `notThere`. The
// hash of the key is in T13 by the time `checkCallee` is called, which is before anything of the table is looked at.
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

    // Its hash, if it is its own normal form: a number that is not an int32 may not be, and what a BigInt is is more than its address.
    CCallHelpers::JumpList hashed;
    CCallHelpers::JumpList plain;
    Jump notCell = jit.branchIfNotCell(key, DoNotHaveTagRegisters);
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
    otherwise.append(jit.branchIfNotInt32(key, DoNotHaveTagRegisters));
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
    // Both are their own normal forms. If they are not the same thing, they are the same key only if both are strings that say
    // the same.
    next.append(jit.branchIfNotCell(entryKey, DoNotHaveTagRegisters));
    next.append(jit.branchIfNotCell(key, DoNotHaveTagRegisters));
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
    // The same hash and as long. Character by character, if both have them a byte each.
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
        otherwise.append(jit.branchIfNotCell(A0, DoNotHaveTagRegisters));
        otherwise.append(jit.branchIfNotType(A0, JSFunctionType));
        jit.loadPtr(Address(A0, JSFunction::offsetOfExecutableOrRareData()), T11);
        Jump hasExecutable = jit.branchTestPtr(CCallHelpers::Zero, T11, TrustedImm32(JSFunction::rareDataTag));
        jit.loadPtr(Address(T11, FunctionRareData::offsetOfExecutable() - JSFunction::rareDataTag), T11);
        hasExecutable.link(&jit);
        // Every kind of executable is big enough to have something there, and only in a native one is it ever the address of a function.
        jit.loadPtr(Address(T11, NativeExecutable::offsetOfNativeFunctionFor(CodeSpecializationKind::CodeForCall)), T11);
        loadInstance(jit, T12);
        jit.loadPtr(Address(T12, Instance::offsetOfRuntimeTable()), T12);
        otherwise.append(jit.branchPtr(CCallHelpers::NotEqual, T11, Address(T12, static_cast<unsigned>(function) * sizeof(void*))));
    };
    auto loadThisOfType = [&](JSType type) {
        jit.move(argument(0), A1);
        otherwise.append(jit.branchIfNotCell(A1, DoNotHaveTagRegisters));
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
        // Of a string that has its characters together, at a place where there is one.
        loadThisOfType(StringType);
        jit.loadPtr(Address(A1, JSString::offsetOfValue()), T13);
        otherwise.append(jit.branchTestPtr(CCallHelpers::NonZero, T13, TrustedImm32(JSString::isRopeInPointer)));
        jit.move(argument(1), A2);
        otherwise.append(jit.branchIfNotInt32(A2, DoNotHaveTagRegisters));
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
            // Half of a pair is the function's to make sense of.
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
        // On to an array that has room, in storage of a kind that the value can go in as it is, and that the collector need not
        // be told of.
        loadThisOfType(ArrayType);
        jit.load8(Address(A1, JSCell::indexingTypeAndMiscOffset()), A3);
        jit.and32(TrustedImm32(IndexingShapeMask | CopyOnWrite), A3);
        jit.move(argument(1), A2);
        Jump holdsValues = jit.branch32(CCallHelpers::Equal, A3, TrustedImm32(ContiguousShape));
        otherwise.append(jit.branch32(CCallHelpers::NotEqual, A3, TrustedImm32(Int32Shape)));
        otherwise.append(jit.branchIfNotInt32(A2, DoNotHaveTagRegisters));
        Jump isInt32 = jit.jump();
        holdsValues.link(&jit);
        Jump notCell = jit.branchIfNotCell(A2, DoNotHaveTagRegisters);
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
        // The last of an array that has one there.
        loadThisOfType(ArrayType);
        jit.load8(Address(A1, JSCell::indexingTypeAndMiscOffset()), A3);
        jit.and32(TrustedImm32(IndexingShapeMask | CopyOnWrite), A3);
        Jump holdsValues = jit.branch32(CCallHelpers::Equal, A3, TrustedImm32(ContiguousShape));
        otherwise.append(jit.branch32(CCallHelpers::NotEqual, A3, TrustedImm32(Int32Shape)));
        holdsValues.link(&jit);
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
        Jump notCell = jit.branchIfNotCell(A1, DoNotHaveTagRegisters);
        jit.load8(Address(A1, JSCell::typeInfoTypeOffset()), A2);
        // What a proxy is is what it stands for, which is the function's to find out.
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
    case StubIntrinsic::SetAndForget:
    case StubIntrinsic::Add:
    case StubIntrinsic::AddAndForget: {
        bool isOfMap = intrinsic == StubIntrinsic::Get || intrinsic == StubIntrinsic::Set || intrinsic == StubIntrinsic::SetAndForget;
        bool isOfSet = intrinsic == StubIntrinsic::Add || intrinsic == StubIntrinsic::AddAndForget;
        // findInMapOrSet() has a use for the registers that `this` and the arguments are kept in, and leaves these alone.
        CCallHelpers::JumpList putBack;
        ifNotTheCallee = &putBack;
        bool hasValue = intrinsic == StubIntrinsic::Set || intrinsic == StubIntrinsic::SetAndForget;
        jit.move(argument(0), A1);
        jit.move(argument(1), A2);
        if (hasValue)
            jit.move(argument(2), A3);
        putBack.append(jit.branchIfNotCell(A1, DoNotHaveTagRegisters));
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
                // The value goes where the one before it was, if the collector need not be told.
                Jump notCell = jit.branchIfNotCell(A3, DoNotHaveTagRegisters);
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

StubIntrinsic stubIntrinsicFor(UniquedStringImpl* name, unsigned argumentCountIncludingThis, bool resultIsWanted)
{
    enum { Any, Wanted, NotWanted };
    if constexpr (!usesStubs)
        return StubIntrinsic::None;
    if (!Options::aotCallIntrinsics() || name->length() > 11 || name->isSymbol())
        return StubIntrinsic::None;
#define AOT_STUB_INTRINSIC_FOR(intrinsic, text, argumentCount, result) \
    if (argumentCount + 1 == argumentCountIncludingThis && (result == Any || (result == Wanted) == resultIsWanted) && WTF::equal(name, text ""_s)) \
        return StubIntrinsic::intrinsic;
    FOR_EACH_AOT_STUB_INTRINSIC(AOT_STUB_INTRINSIC_FOR)
#undef AOT_STUB_INTRINSIC_FOR
    return StubIntrinsic::None;
}


#else // CPU(ARM64)

#define AOT_NO_STUB(name) static void generate##name(CCallHelpers& jit) { jit.breakpoint(); }
FOR_EACH_AOT_STUB(AOT_NO_STUB)
#undef AOT_NO_STUB

#endif // CPU(ARM64)

// What calls an operation is told which by where it is in the runtime's table. What calls a function is told how many arguments.
static constexpr Stub stubsThatCallOperations[] = {
    Stub::OperationValue, Stub::OperationVoid, Stub::OperationDouble, Stub::OperationValueWithGlobalObject, Stub::OperationVoidWithGlobalObject, Stub::OperationDoubleWithGlobalObject,
    Stub::PlainOperation, Stub::PlainOperationWithGlobalObject, Stub::PlainOperationWithVM,
    Stub::ColdOperationVoid, Stub::ColdOperationVoidOfLeaf, Stub::ColdOperationValue, Stub::ColdOperationValueOfLeaf,
};
static constexpr Stub stubsThatCallFunctions[] = { Stub::Call, Stub::Construct };
static constexpr unsigned numberOfCountsWithThunk = numberOfArgumentGPRs + 1; // From none to as many as there are registers for.
// The prologue is told how big the frame is, which is a multiple of this.
static constexpr unsigned unitOfFrameSize = stackAlignmentBytes();
static constexpr unsigned biggestFrameWithThunk = 64 * unitOfFrameSize;
static constexpr unsigned firstThunkOfCalls = std::size(stubsThatCallOperations) * numberOfEntries;
static constexpr unsigned firstThunkOfPrologue = firstThunkOfCalls + std::size(stubsThatCallFunctions) * numberOfCountsWithThunk;
static constexpr unsigned firstThunkOfIntrinsics = firstThunkOfPrologue + biggestFrameWithThunk / unitOfFrameSize;
static constexpr unsigned numberOfThunks = firstThunkOfIntrinsics + numberOfStubIntrinsics;

std::optional<unsigned> thunkFor(Stub stub, uint32_t valueOfT9)
{
    if constexpr (!usesStubs)
        return std::nullopt;
    for (unsigned i = 0; i < std::size(stubsThatCallOperations); ++i) {
        if (stubsThatCallOperations[i] == stub)
            return valueOfT9 % sizeof(void*) || valueOfT9 / sizeof(void*) >= numberOfEntries ? std::nullopt : std::optional<unsigned> { i * numberOfEntries + valueOfT9 / sizeof(void*) };
    }
    for (unsigned i = 0; i < std::size(stubsThatCallFunctions); ++i) {
        if (stubsThatCallFunctions[i] == stub)
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
            for (Stub stub : stubsThatCallOperations) {
                for (unsigned entry = 0; entry < numberOfEntries; ++entry) {
                    thunkLabels.append(jit.label());
                    jit.move(CCallHelpers::TrustedImm32(entry * sizeof(void*)), GPRInfo::regT9);
                    jit.jump().linkTo(labels[static_cast<unsigned>(stub)], &jit);
                }
            }
            // These are on the way of everything a program does, and short: each has the stub all over again, right after it.
            for (Stub stub : stubsThatCallFunctions) {
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
                if (Options::aotCallIntrinsicsMustBeRight() & (1u << i)) [[unlikely]]
                    jit.breakpoint();
                jit.move(T14, argumentGPR(0));
                jit.move(T15, argumentGPR(1));
                static_assert(stubsThatCallFunctions[0] == Stub::Call);
                jit.jump().linkTo(thunkLabels[firstThunkOfCalls + argumentCountOf(intrinsic)], &jit);
            }
            RELEASE_ASSERT(thunkLabels.size() == numberOfThunks);
        }

        LinkBuffer linkBuffer(jit, GLOBAL_THUNK_ID, LinkBuffer::Profile::Thunk);
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
        static NeverDestroyed<MacroAssemblerCodeRef<JITThunkPtrTag>> code;
        code.get() = FINALIZE_THUNK(linkBuffer, JITThunkPtrTag, "AOTStubs"_s, "Stubs of the static compiler");
        blob->inJITMemory = start;
        blob->bytes.append(std::span { start, size });
        if (Options::aotVerbose()) [[unlikely]] {
            static constexpr ASCIILiteral names[] = {
#define AOT_STUB_NAME(name) #name ""_s,
                FOR_EACH_AOT_STUB(AOT_STUB_NAME)
#undef AOT_STUB_NAME
            };
            for (unsigned i = 0; i < numberOfStubs; ++i)
                dataLogLn("AOT: stub ", names[i], " ", blob->offsets[i]);
            // (In the order they are made in, above.)
            unsigned thunk = 0;
            for (Stub stub : stubsThatCallOperations) {
                dataLogLn("AOT: stub ThunksOf", names[static_cast<unsigned>(stub)], " ", blob->thunkOffsets[thunk]);
                thunk += numberOfEntries;
            }
            for (Stub stub : stubsThatCallFunctions) {
                for (unsigned count = 0; count < numberOfCountsWithThunk; ++count)
                    dataLogLn("AOT: stub ", names[static_cast<unsigned>(stub)], count, " ", blob->thunkOffsets[thunk++]);
            }
            for (unsigned frameSize = unitOfFrameSize; frameSize <= biggestFrameWithThunk; frameSize += unitOfFrameSize)
                dataLogLn("AOT: stub Prologue", frameSize, " ", blob->thunkOffsets[thunk++]);
            for (unsigned i = 1; i <= numberOfStubIntrinsics; ++i)
                dataLogLn("AOT: stub Intrinsic", i, " ", blob->thunkOffsets[thunk++]);
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
    // (Half a word at a time is what is loaded, if that is how far apart they are.)
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
    // add dest, base, #high, lsl #12; ldr dest, [dest, #low]. Both have twelve bits for it in the same place, and the second counts in words.
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
    const StubBlob& blob = stubBlob();
    auto* start = static_cast<uint8_t*>(linkBuffer.entrypoint<JSEntryPtrTag>().untaggedPtr());
    Vector<StubCall> result;
    for (auto& pending : m_pending) {
        void* target = static_cast<uint8_t*>(blob.inJITMemory) + (pending.thunk ? blob.thunkOffsets[pending.thunk - 1] : blob.offsets[static_cast<unsigned>(pending.stub)]);
        linkBuffer.link<JITThunkPtrTag>(pending.call, CodeLocationLabel<JITThunkPtrTag>(tagCodePtr<JITThunkPtrTag>(target)));
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
