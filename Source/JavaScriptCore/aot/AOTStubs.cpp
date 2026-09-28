/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubs.h"

#if ENABLE(FTL_JIT)

#include "AOTRuntime.h"
#include "AOTThunks.h"
#include "BaselineJITRegisters.h"
#include "CodeBlock.h"
#include "GetterSetter.h"
#include "JSGlobalObject.h"
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

static void jumpToEntry(CCallHelpers& jit, GPRReg data, Entry entry)
{
    jit.loadPtr(Address(data, Instance::offsetOfRuntimeTable()), data);
    jit.loadPtr(Address(data, static_cast<unsigned>(entry) * sizeof(void*)), data);
    jit.farJump(data, JITThunkPtrTag);
}

static void generatePrologue(CCallHelpers& jit)
{
    jit.loadPtr(CCallHelpers::addressFor(CallFrameSlot::codeBlock), GPRInfo::regT0);
    jit.loadPtr(Address(GPRInfo::regT0, Instance::offsetOfVM()), GPRInfo::regT1);
    jit.subPtr(GPRInfo::callFrameRegister, GPRInfo::regT9, GPRInfo::regT2);
    jit.loadPtr(Address(GPRInfo::regT1, VM::offsetOfSoftStackLimit()), GPRInfo::regT3);
    Jump overflow = jit.branchPtr(CCallHelpers::Above, GPRInfo::regT3, GPRInfo::regT2);
    // A frame so big that the subtraction wrapped around.
    Jump wrapped = jit.branchPtr(CCallHelpers::Above, GPRInfo::regT2, GPRInfo::callFrameRegister);
    jit.move(GPRInfo::regT2, CCallHelpers::stackPointerRegister);
    jit.ret();

    overflow.link(&jit);
    wrapped.link(&jit);
    jumpToEntry(jit, GPRInfo::regT0, Entry::ThrowStackOverflowAtPrologue);
}

static void generateArityCheck(CCallHelpers& jit)
{
#if CPU(ARM64)
    // Nothing has been pushed: the frame's slots are found from the stack pointer.
    auto slot = [](CallFrameSlot slot) {
        return CCallHelpers::calleeFrameSlot(slot).withOffset(sizeof(CallerFrameAndPC) - prologueStackPointerDelta());
    };
    auto comeBack = [&] {
        jit.move(CCallHelpers::linkRegister, GPRInfo::regT11);
        jit.move(GPRInfo::regT10, CCallHelpers::linkRegister);
        jit.farJump(GPRInfo::regT11, NoPtrTag);
    };
    jit.load32(slot(CallFrameSlot::argumentCountIncludingThis).withOffset(LowWordOffset), GPRInfo::argumentGPR2);
    Jump tooFew = jit.branch32(CCallHelpers::Below, GPRInfo::argumentGPR2, GPRInfo::regT9);
    comeBack();

    tooFew.link(&jit);
    // The padding: align2(numParameters + header) - (argumentCountIncludingThis + header). The header is an odd number of slots.
    static_assert(stackAlignmentRegisters() == 2 && (CallFrame::headerSizeInRegisters & 1));
    jit.or32(TrustedImm32(1), GPRInfo::regT9, GPRInfo::argumentGPR0);
    jit.sub32(GPRInfo::argumentGPR2, GPRInfo::argumentGPR0);

    // Is there stack for it?
    jit.add32(TrustedImm32(1), GPRInfo::argumentGPR0, GPRInfo::regT3);
    jit.and32(TrustedImm32(~1U), GPRInfo::regT3);
    jit.lshiftPtr(TrustedImm32(3), GPRInfo::regT3);
    jit.subPtr(CCallHelpers::stackPointerRegister, GPRInfo::regT3, GPRInfo::regT3);
    jit.loadPtr(slot(CallFrameSlot::codeBlock), GPRInfo::regT5);
    jit.loadPtr(Address(GPRInfo::regT5, Instance::offsetOfVM()), GPRInfo::regT6);
    Jump overflow = jit.branchPtr(CCallHelpers::Above, Address(GPRInfo::regT6, VM::offsetOfSoftStackLimit()), GPRInfo::regT3);

    // From here on, what LLInt::arityFixupThunk() does.
    jit.subPtr(CCallHelpers::stackPointerRegister, TrustedImm32(static_cast<int32_t>(sizeof(CallerFrameAndPC))), GPRInfo::regT3);
    jit.add32(TrustedImm32(CallFrame::headerSizeInRegisters), GPRInfo::argumentGPR2);
    Jump noExtraSlot = jit.branchTest32(CCallHelpers::Zero, GPRInfo::argumentGPR0, TrustedImm32(stackAlignmentRegisters() - 1));
    jit.move(CCallHelpers::TrustedImm64(JSValue::ValueUndefined), GPRInfo::regT5);
    jit.store64(GPRInfo::regT5, CCallHelpers::BaseIndex(GPRInfo::regT3, GPRInfo::argumentGPR2, CCallHelpers::TimesEight));
    jit.add32(TrustedImm32(1), GPRInfo::argumentGPR2);
    jit.and32(TrustedImm32(-stackAlignmentRegisters()), GPRInfo::argumentGPR0);
    Jump done = jit.branchTest32(CCallHelpers::Zero, GPRInfo::argumentGPR0);
    noExtraSlot.link(&jit);

    jit.neg64(GPRInfo::argumentGPR0);
    jit.lshift64(GPRInfo::argumentGPR0, TrustedImm32(3), GPRInfo::regT5);
    jit.addPtr(GPRInfo::regT5, CCallHelpers::stackPointerRegister);
    jit.addPtr(GPRInfo::regT3, GPRInfo::regT5);
    CCallHelpers::Label copyLoop(jit.label());
    jit.loadPair64(CCallHelpers::PostIndexAddress(GPRInfo::regT3, 16), GPRInfo::regT7, GPRInfo::regT6);
    jit.storePair64(GPRInfo::regT7, GPRInfo::regT6, CCallHelpers::PostIndexAddress(GPRInfo::regT5, 16));
    jit.branchSub32(CCallHelpers::NonZero, TrustedImm32(2), GPRInfo::argumentGPR2).linkTo(copyLoop, &jit);
    jit.move(CCallHelpers::TrustedImm64(JSValue::ValueUndefined), GPRInfo::regT7);
    CCallHelpers::Label fillLoop(jit.label());
    jit.storePair64(GPRInfo::regT7, GPRInfo::regT7, CCallHelpers::PostIndexAddress(GPRInfo::regT5, 16));
    jit.branchAdd32(CCallHelpers::NonZero, TrustedImm32(2), GPRInfo::argumentGPR0).linkTo(fillLoop, &jit);
    done.link(&jit);
    comeBack();

    overflow.link(&jit);
    jit.move(GPRInfo::regT10, CCallHelpers::linkRegister);
    jit.emitFunctionPrologue();
    jumpToEntry(jit, GPRInfo::regT5, Entry::ThrowStackOverflowAtPrologue);
#else
    jit.breakpoint();
#endif
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

static void loadInstance(CCallHelpers& jit, GPRReg result)
{
    jit.loadPtr(CCallHelpers::addressFor(CallFrameSlot::codeBlock), result);
}

// boxed: what a frame has for a callee. Leaves the function's Data in `result`.
static void loadDataOf(CCallHelpers& jit, GPRReg boxed, GPRReg instance, GPRReg result)
{
    ASSERT(boxed != instance && result != instance);
    // CalleeBits::asNativeCallee(), and the tag is what it is short of the index.
    jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(lowestAccessibleAddress()) - JSValue::NativeCalleeTag + OBJECT_OFFSETOF(CodeHeader, index)), result);
    jit.load32(CCallHelpers::BaseIndex(boxed, result, CCallHelpers::TimesOne), result);
    jit.lshiftPtr(TrustedImm32(3), result);
    jit.addPtr(instance, result);
    jit.loadPtr(Address(result, Instance::offsetOfData()), result);
}

// The Data of the function whose frame this is. Leaves its Instance in `instance`.
static void loadInstanceAndData(CCallHelpers& jit, GPRReg instance, GPRReg data)
{
    loadInstance(jit, instance);
    jit.load64(CCallHelpers::addressFor(CallFrameSlot::callee), data);
    // (loadDataOf() wants them apart.)
    jit.move(data, CCallHelpers::memoryTempRegister);
    loadDataOf(jit, CCallHelpers::memoryTempRegister, instance, data);
}

static void storeCallSite(CCallHelpers& jit, GPRReg bits)
{
    jit.store32(bits, CCallHelpers::highWordFor(CallFrameSlot::argumentCountIncludingThis));
}

enum class Returns : uint8_t { Value, Void, Double };

// The function is in `function`, its arguments are in place. There is no frame here for C++ to walk through: the frame pointer stays
// the calling function's, which is the frame the operation takes itself to have been called from.
static void callAndCheckException(CCallHelpers& jit, GPRReg function, Returns returns)
{
    jit.pushPair(CCallHelpers::framePointerRegister, CCallHelpers::linkRegister);
    jit.call(function, OperationPtrTag);
    jit.popPair(CCallHelpers::framePointerRegister, CCallHelpers::linkRegister);
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
    storeCallSite(jit, T10);
    loadInstance(jit, T11);
    if (withGlobalObject)
        jit.loadPtr(Address(T11, Instance::offsetOfGlobalObject()), A0);
    jit.loadPtr(Address(T11, Instance::offsetOfRuntimeTable()), T11);
    jit.loadPtr(CCallHelpers::BaseIndex(T11, T9, CCallHelpers::TimesOne), T11);
    callAndCheckException(jit, T11, returns);
}

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
    storeCallSite(jit, T10);
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
    storeCallSite(jit, T10);
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

static void generateGetByVal(CCallHelpers& jit)
{
    constexpr FPRReg number = FPRInfo::fpRegT0;
    CCallHelpers::JumpList slow;
    jit.move(CCallHelpers::TrustedImm64(JSValue::NumberTag), T9);
    slow.append(jit.branch64(CCallHelpers::Below, A1, T9));
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
    storeCallSite(jit, T10);
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
    loadInstanceAndData(jit, T9, T10);
    siteOfSlot(jit, T10, site, T13);
    jit.loadPtr(Address(T10, Data::offsetOfSites()), T11);
    static_assert(sizeof(Site) == 8);
    jit.load32(CCallHelpers::BaseIndex(T11, T13, CCallHelpers::TimesEight, OBJECT_OFFSETOF(Site, callSiteBits)), T10);
    storeCallSite(jit, T10);
    jit.load32(CCallHelpers::BaseIndex(T11, T13, CCallHelpers::TimesEight, OBJECT_OFFSETOF(Site, identifierAndExtra)), T12);
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

    // What is there is a GetterSetter. If its getter is a function that has code it is called from here, and returns to whoever
    // called the stub, which has room for its frame and puts the stack pointer back afterwards.
    isGetter.link(&jit);
    locateCachedProperty(jit, T12, T11, T13);
    jit.load64(CCallHelpers::BaseIndex(T13, T11, CCallHelpers::TimesEight), T12);
    jit.loadPtr(Address(T12, GetterSetter::offsetOfGetter()), T12);
    miss.append(jit.branchIfNotType(T12, JSFunctionType));
    jit.loadPtr(Address(T12, JSFunction::offsetOfExecutableOrRareData()), T11);
    Jump hasExecutable = jit.branchTestPtr(CCallHelpers::Zero, T11, TrustedImm32(JSFunction::rareDataTag));
    jit.loadPtr(Address(T11, FunctionRareData::offsetOfExecutable() - JSFunction::rareDataTag), T11);
    hasExecutable.link(&jit);
    jit.loadPtr(Address(T11, ExecutableBase::offsetOfJITCodeWithArityCheckFor(CodeSpecializationKind::CodeForCall)), T13);
    miss.append(jit.branchTestPtr(CCallHelpers::Zero, T13));
    Jump isNative = jit.branchIfNotType(T11, FunctionExecutableType);
    jit.loadPtr(Address(T11, FunctionExecutable::offsetOfCodeBlockFor(CodeSpecializationKind::CodeForCall)), T11);
    jit.storePtr(T11, slotOfFrameBeingMade(CallFrameSlot::codeBlock));
    isNative.link(&jit);
    jit.store64(A0, slotOfFrameBeingMade(CallFrameSlot::thisArgument));
    jit.store64(T12, slotOfFrameBeingMade(CallFrameSlot::callee));
    jit.store32(TrustedImm32(1), slotOfFrameBeingMade(CallFrameSlot::argumentCountIncludingThis, LowWordOffset));
    loadInstanceAndData(jit, T11, T9);
    siteOfSlot(jit, T9, A1, T10);
    jit.loadPtr(Address(T9, Data::offsetOfSites()), T11);
    jit.load32(CCallHelpers::BaseIndex(T11, T10, CCallHelpers::TimesEight, OBJECT_OFFSETOF(Site, callSiteBits)), T11);
    storeCallSite(jit, T11);
    jit.move(T12, BaselineJITRegisters::Call::calleeGPR);
    jit.farJump(T13, JSEntryPtrTag);

    miss.link(&jit);
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
    jit.subPtr(TrustedImm32(16), CCallHelpers::stackPointerRegister);
    jit.storePtr(CCallHelpers::linkRegister, Address(CCallHelpers::stackPointerRegister));
    jit.storePtr(A1, Address(CCallHelpers::stackPointerRegister, 8));
    prepareMissAtSite(jit, Entry::operationAOTResolveScope, 1);
    jit.call(T9, OperationPtrTag);
    jit.move(GPRInfo::returnValueGPR2, T11);
    jit.loadPtr(Address(CCallHelpers::stackPointerRegister), CCallHelpers::linkRegister);
    jit.loadPtr(Address(CCallHelpers::stackPointerRegister, 8), A1);
    jit.addPtr(TrustedImm32(16), CCallHelpers::stackPointerRegister);
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

// vm: the VM. lookUp: operationLookupExceptionHandler(), or the one that starts from the caller's frame. Both in registers that a
// call does not preserve. Does not come back.
static void unwind(CCallHelpers& jit, GPRReg vm, GPRReg lookUp)
{
    constexpr GPRReg keptVM = GPRInfo::regCS0;
    RELEASE_ASSERT(noOverlap(vm, lookUp, A0, GPRInfo::regT1));
    jit.move(vm, GPRInfo::regT1);
    jit.copyCalleeSavesToVMEntryFrameCalleeSavesBuffer(GPRInfo::regT1);
    // Which leaves them free to use: the handler gets them from there.
    jit.move(vm, keptVM);
    jit.move(vm, A0);
    jit.call(lookUp, OperationPtrTag);
    // genericUnwind() leaves the handler's frame in VM::callFrameForCatch, and where to go in VM::targetMachinePCForThrow.
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

static void generateThrowStackOverflowAtPrologue(CCallHelpers& jit)
{
    constexpr GPRReg keptInstance = GPRInfo::regCS0; // Nothing returns from here to anyone who minds.
    // The first call site of any function is its beginning.
    jit.store32(TrustedImm32(0), CCallHelpers::highWordFor(CallFrameSlot::argumentCountIncludingThis));
    loadInstance(jit, T9);
    jit.loadPtr(Address(T9, Instance::offsetOfRuntimeTable()), T9);
    jit.loadPtr(Address(T9, static_cast<unsigned>(Entry::ThrowStackOverflowError) * sizeof(void*)), T9);
    // The frame is not yet one of the function's (see compile()), and is about to be nobody's.
    jit.loadPtr(CCallHelpers::addressFor(CallFrameSlot::codeBlock), T10);
    jit.move(T10, keptInstance);
    loadDataOf(jit, boxedHeaderGPR, T10, A0);
    jit.loadPtr(Address(A0, OBJECT_OFFSETOF(Data, codeBlock)), A0);
    jit.call(T9, OperationPtrTag);

    jit.move(keptInstance, T9);
    jit.loadPtr(Address(T9, Instance::offsetOfVM()), T10);
    jit.loadPtr(Address(T9, Instance::offsetOfRuntimeTable()), T9);
    jit.loadPtr(Address(T9, static_cast<unsigned>(Entry::LookupExceptionHandlerFromCallerFrame) * sizeof(void*)), T11);
    unwind(jit, T10, T11);
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

// Where whoever calls a function the way any function is called ends up: the interpreter, C++, a stub that was given an object to
// call. The frame is made, nothing has been pushed, and it has the function's CodeBlock, as a frame of the interpreter's would.
static void generateEnter(CCallHelpers& jit)
{
    jit.loadPtr(slotOfFrameBeingMade(CallFrameSlot::codeBlock), T9);
    jit.loadPtr(Address(T9, CodeBlock::offsetOfGlobalObject()), T10);
    jit.loadPtr(Address(T9, CodeBlock::jitCodeOffset()), T9);
    jit.loadPtr(Address(T10, JSGlobalObject::offsetOfAOTInstance()), T10);
    jit.loadPtr(Address(T9, JITCode::offsetOfEntry()), T9);
    jit.storePtr(T10, slotOfFrameBeingMade(CallFrameSlot::codeBlock));
    jit.farJump(T9, JSEntryPtrTag);
}

// ---- Calls

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

static void generateVirtualCall(CCallHelpers& jit) { dispatchCall(jit, CodeSpecializationKind::CodeForCall, [] { }); }
static void generateVirtualConstruct(CCallHelpers& jit) { dispatchCall(jit, CodeSpecializationKind::CodeForConstruct, [] { }); }
static void generateVirtualTailCall(CCallHelpers& jit) { dispatchCall(jit, CodeSpecializationKind::CodeForCall, [] { }); }

static void generateCallTo(CCallHelpers& jit, Entry callLinkInfo, CodeSpecializationKind kind)
{
    jit.store64(BaselineJITRegisters::Call::calleeGPR, slotOfFrameBeingMade(CallFrameSlot::callee));
    jit.store32(T9, slotOfFrameBeingMade(CallFrameSlot::argumentCountIncludingThis, LowWordOffset));
    storeCallSite(jit, T10);
    dispatchCall(jit, kind, [&] {
        loadInstance(jit, T11);
        jit.loadPtr(Address(T11, Instance::offsetOfRuntimeTable()), T11);
        jit.loadPtr(Address(T11, static_cast<unsigned>(callLinkInfo) * sizeof(void*)), BaselineJITRegisters::Call::callLinkInfoGPR);
    });
}

static void generateCallAndLinkTo(CCallHelpers& jit, Entry callLinkInfo, CodeSpecializationKind kind)
{
    static_assert(BaselineJITRegisters::Call::calleeGPR == A0);
    jit.store64(A0, slotOfFrameBeingMade(CallFrameSlot::callee));
    jit.store32(T9, slotOfFrameBeingMade(CallFrameSlot::argumentCountIncludingThis, LowWordOffset));
    storeCallSite(jit, T10);

    // The callee may be the one after all, now that it has been called and has code. It is only asked so often.
    jit.load32(slotWord(A1, 0).withOffset(sizeof(uint32_t)), T11);
    jit.and32(TrustedImm32(Slot::attemptsMask), T11);
    Jump gaveUp = jit.branch32(CCallHelpers::Equal, T11, TrustedImm32(Slot::attemptsMask));
    jit.subPtr(TrustedImm32(16), CCallHelpers::stackPointerRegister);
    jit.storePtr(CCallHelpers::linkRegister, Address(CCallHelpers::stackPointerRegister));
    jit.storePtr(A0, Address(CCallHelpers::stackPointerRegister, 8));
    prepareMissAtSite(jit, Entry::operationAOTLinkCall, 1);
    jit.call(T9, OperationPtrTag);
    jit.loadPtr(Address(CCallHelpers::stackPointerRegister), CCallHelpers::linkRegister);
    jit.loadPtr(Address(CCallHelpers::stackPointerRegister, 8), A0);
    jit.addPtr(TrustedImm32(16), CCallHelpers::stackPointerRegister);
    gaveUp.link(&jit);

    dispatchCall(jit, kind, [&] {
        loadInstance(jit, T11);
        jit.loadPtr(Address(T11, Instance::offsetOfRuntimeTable()), T11);
        jit.loadPtr(Address(T11, static_cast<unsigned>(callLinkInfo) * sizeof(void*)), BaselineJITRegisters::Call::callLinkInfoGPR);
    });
}

static void generateCallAndLink(CCallHelpers& jit) { generateCallAndLinkTo(jit, Entry::CallLinkInfoForCall, CodeSpecializationKind::CodeForCall); }
static void generateConstructAndLink(CCallHelpers& jit) { generateCallAndLinkTo(jit, Entry::CallLinkInfoForConstruct, CodeSpecializationKind::CodeForConstruct); }

static void generateCallFarFunctionTo(CCallHelpers& jit, Entry callLinkInfo, CodeSpecializationKind kind)
{
    dispatchCall(jit, kind, [&] {
        loadInstance(jit, T11);
        jit.loadPtr(Address(T11, Instance::offsetOfRuntimeTable()), T11);
        jit.loadPtr(Address(T11, static_cast<unsigned>(callLinkInfo) * sizeof(void*)), BaselineJITRegisters::Call::callLinkInfoGPR);
    });
}

static void generateCallFarFunction(CCallHelpers& jit) { generateCallFarFunctionTo(jit, Entry::CallLinkInfoForCall, CodeSpecializationKind::CodeForCall); }
static void generateConstructFarFunction(CCallHelpers& jit) { generateCallFarFunctionTo(jit, Entry::CallLinkInfoForConstruct, CodeSpecializationKind::CodeForConstruct); }

static void generateTailCallPrepare(CCallHelpers& jit)
{
    constexpr GPRReg callee = BaselineJITRegisters::Call::calleeGPR;
    jit.store64(callee, slotOfFrameBeingMade(CallFrameSlot::callee));
    jit.store32(T9, slotOfFrameBeingMade(CallFrameSlot::argumentCountIncludingThis, LowWordOffset));
    storeCallSite(jit, T10);

    // Leaves the callee's code in T12 and its CodeBlock in the frame, or goes to `otherwise`.
    auto findCode = [&](CCallHelpers::JumpList& otherwise) {
        otherwise.append(jit.branchIfNotCell(callee, DoNotHaveTagRegisters));
        otherwise.append(jit.branchIfNotType(callee, JSFunctionType));
        jit.loadPtr(Address(callee, JSFunction::offsetOfExecutableOrRareData()), T11);
        Jump hasExecutable = jit.branchTestPtr(CCallHelpers::Zero, T11, TrustedImm32(JSFunction::rareDataTag));
        jit.loadPtr(Address(T11, FunctionRareData::offsetOfExecutable() - JSFunction::rareDataTag), T11);
        hasExecutable.link(&jit);
        jit.loadPtr(Address(T11, ExecutableBase::offsetOfJITCodeWithArityCheckFor(CodeSpecializationKind::CodeForCall)), T12);
        otherwise.append(jit.branchTestPtr(CCallHelpers::Zero, T12));
        Jump isNative = jit.branchIfNotType(T11, FunctionExecutableType);
        jit.loadPtr(Address(T11, FunctionExecutable::offsetOfCodeBlockFor(CodeSpecializationKind::CodeForCall)), T11);
        jit.storePtr(T11, slotOfFrameBeingMade(CallFrameSlot::codeBlock));
        isNative.link(&jit);
        jit.ret();
    };
    CCallHelpers::JumpList slow;
    findCode(slow);

    // While there still is a frame to say who is calling.
    slow.link(&jit);
    loadInstance(jit, T11);
    jit.move(callee, A1);
    jit.loadPtr(Address(T11, Instance::offsetOfGlobalObject()), A0);
    jit.loadPtr(Address(T11, Instance::offsetOfRuntimeTable()), T11);
    jit.loadPtr(Address(T11, static_cast<unsigned>(Entry::operationAOTPrepareTailCall) * sizeof(void*)), T11);
    jit.pushPair(CCallHelpers::framePointerRegister, CCallHelpers::linkRegister);
    jit.call(T11, OperationPtrTag);
    jit.popPair(CCallHelpers::framePointerRegister, CCallHelpers::linkRegister);
    Jump exception = jit.branchTestPtr(CCallHelpers::NonZero, GPRInfo::returnValueGPR2);
    jit.move(GPRInfo::returnValueGPR, T9);
    jit.load64(slotOfFrameBeingMade(CallFrameSlot::callee), callee);
    CCallHelpers::JumpList cannot;
    cannot.append(jit.branchTestPtr(CCallHelpers::Zero, T9));
    findCode(cannot);
    cannot.link(&jit);
    jit.move(TrustedImm32(0), T12);
    jit.ret();

    exception.link(&jit);
    loadInstance(jit, T9);
    jumpToEntry(jit, T9, Entry::HandleException);
}

static void generateTailCallFinish(CCallHelpers& jit)
{
    jit.prepareForTailCallSlow(RegisterSet { BaselineJITRegisters::Call::calleeGPR, T12 });
    jit.farJump(T12, JSEntryPtrTag);
}

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
        storeCallSite(jit, T10);
        jit.subPtr(TrustedImm32(32), CCallHelpers::stackPointerRegister);
        jit.storePtr(CCallHelpers::linkRegister, Address(CCallHelpers::stackPointerRegister));
        jit.store64(kept, Address(CCallHelpers::stackPointerRegister, 8));
        loadInstance(jit, T11);
        setUp();
        jit.loadPtr(Address(T11, Instance::offsetOfGlobalObject()), A0);
        jit.loadPtr(Address(T11, Instance::offsetOfRuntimeTable()), T11);
        jit.loadPtr(Address(T11, static_cast<unsigned>(operation) * sizeof(void*)), T11);
        jit.call(T11, OperationPtrTag);
        jit.loadPtr(Address(CCallHelpers::stackPointerRegister), CCallHelpers::linkRegister);
        jit.load64(Address(CCallHelpers::stackPointerRegister, 8), A2);
        jit.addPtr(TrustedImm32(32), CCallHelpers::stackPointerRegister);
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
        jit.addPtr(TrustedImm32(8), CCallHelpers::stackPointerRegister, A2);
    });

    nextIsCell.link(&jit);
    generic.append(jit.branchIfNotType(A0, SentinelType));
    callKeeping(Entry::operationAOTIteratorNextTryFast, A0, [&] { });

    generic.link(&jit);
    jit.move(TrustedImm32(1), T9);
    jit.ret();
}

static void generateCall(CCallHelpers& jit) { generateCallTo(jit, Entry::CallLinkInfoForCall, CodeSpecializationKind::CodeForCall); }
static void generateConstruct(CCallHelpers& jit) { generateCallTo(jit, Entry::CallLinkInfoForConstruct, CodeSpecializationKind::CodeForConstruct); }

#else // CPU(ARM64)

#define AOT_NO_STUB(name) static void generate##name(CCallHelpers& jit) { jit.breakpoint(); }
FOR_EACH_AOT_STUB(AOT_NO_STUB)
#undef AOT_NO_STUB

#endif // CPU(ARM64)

const StubBlob& stubBlob()
{
    static LazyNeverDestroyed<StubBlob> blob;
    static std::once_flag once;
    std::call_once(once, [] {
        blob.construct();
        CCallHelpers jit;
        CCallHelpers::Label labels[numberOfStubs];
#define AOT_GENERATE_STUB(name) \
        jit.align(); \
        labels[static_cast<unsigned>(Stub::name)] = jit.label(); \
        generate##name(jit);
        FOR_EACH_AOT_STUB(AOT_GENERATE_STUB)
#undef AOT_GENERATE_STUB

        LinkBuffer linkBuffer(jit, GLOBAL_THUNK_ID, LinkBuffer::Profile::Thunk);
        auto* start = static_cast<uint8_t*>(linkBuffer.entrypoint<JITThunkPtrTag>().untaggedPtr());
        for (unsigned i = 0; i < numberOfStubs; ++i)
            blob->offsets[i] = static_cast<uint8_t*>(linkBuffer.locationOf<JITThunkPtrTag>(labels[i]).untaggedPtr()) - start;
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
        }
    });
    return blob.get();
}

void StubCalls::call(CCallHelpers& jit, Stub stub)
{
    m_pending.append({ jit.nearCall(), stub, false });
}

void StubCalls::callFunction(CCallHelpers& jit, Stub otherwise, uint32_t knownCallee, bool skipsArityCheck)
{
    m_pending.append({ jit.nearCall(), otherwise, false, skipsArityCheck, knownCallee });
}

void StubCalls::tailCall(CCallHelpers& jit, Stub stub)
{
    m_pending.append({ jit.nearTailCall(), stub, true });
}

void HeaderReferences::moveBoxedHeader(CCallHelpers& jit, GPRReg reg)
{
    m_references.append({ jit.label(), reg, false });
    jit.nop(); // adr reg, header + NativeCalleeTag
    jit.move(CCallHelpers::TrustedImm64(lowestAccessibleAddress()), CCallHelpers::dataTempRegister);
    jit.subPtr(CCallHelpers::dataTempRegister, reg);
}

void HeaderReferences::loadIndex(CCallHelpers& jit, GPRReg reg)
{
    m_references.append({ jit.label(), reg, true });
    jit.nop(); // ldr reg, header.index
}

void HeaderReferences::link(LinkBuffer& linkBuffer, CCallHelpers::Label header)
{
#if CPU(ARM64)
    auto* target = static_cast<uint8_t*>(linkBuffer.locationOf<JSEntryPtrTag>(header).untaggedPtr());
    for (auto& reference : m_references) {
        auto* instruction = static_cast<uint8_t*>(linkBuffer.locationOf<JSEntryPtrTag>(reference.instruction).untaggedPtr());
        uint32_t encoded;
        if (reference.isLoadOfIndex) {
            int64_t delta = (target + OBJECT_OFFSETOF(CodeHeader, index) - instruction) / 4;
            RELEASE_ASSERT(delta >= -(1 << 18) && delta < (1 << 18));
            encoded = 0x18000000u | (static_cast<uint32_t>(delta) & 0x7ffffu) << 5 | static_cast<uint32_t>(reference.reg);
        } else {
            int64_t delta = target + JSValue::NativeCalleeTag - instruction;
            RELEASE_ASSERT(delta >= -(1 << 20) && delta < (1 << 20));
            encoded = 0x10000000u | (static_cast<uint32_t>(delta) & 3u) << 29 | (static_cast<uint32_t>(delta >> 2) & 0x7ffffu) << 5 | static_cast<uint32_t>(reference.reg);
        }
        performJITMemcpy<jitMemcpyRepatch>(instruction, &encoded, sizeof(encoded));
    }
#else
    UNUSED_PARAM(linkBuffer);
    UNUSED_PARAM(header);
    RELEASE_ASSERT_NOT_REACHED();
#endif
}

Vector<StubCall> StubCalls::link(LinkBuffer& linkBuffer)
{
    const StubBlob& blob = stubBlob();
    auto* start = static_cast<uint8_t*>(linkBuffer.entrypoint<JSEntryPtrTag>().untaggedPtr());
    Vector<StubCall> result;
    for (auto& pending : m_pending) {
        void* target = static_cast<uint8_t*>(blob.inJITMemory) + blob.offsets[static_cast<unsigned>(pending.stub)];
        linkBuffer.link<JITThunkPtrTag>(pending.call, CodeLocationLabel<JITThunkPtrTag>(tagCodePtr<JITThunkPtrTag>(target)));
        // The location of a near call is the end of the instruction; that of a near tail call is the instruction.
        auto* location = static_cast<uint8_t*>(linkBuffer.locationOfNearCall<JITThunkPtrTag>(pending.call).dataLocation());
        result.append({ static_cast<uint32_t>(location - start - (pending.isTailCall ? 0 : sizeof(uint32_t))), pending.stub, pending.isTailCall, pending.skipsArityCheck, pending.function });
    }
    return result;
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
