/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsConstruct.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTInstance.h"
#include "AOTStubFrontEnds.h"
#include "AOTStubHelpers.h"
#include "FunctionRareData.h"
#include "InternalFunction.h"
#include "JSFunction.h"
#include "NativeExecutable.h"
#include "StackAlignment.h"
#include "StackManager.h"
#include "VM.h"

namespace JSC { namespace AOT {

#if CPU(X86_64)
static_assert(gprOf(StubRegister::T10) == gprOf(StubRegister::A4));
static constexpr StubRegister globalObjectRegister = StubRegister::R0;
#else
static_assert(gprOf(StubRegister::R0) == gprOf(StubRegister::A0));
static constexpr StubRegister globalObjectRegister = StubRegister::T10;
#endif

struct ConstructOperands {
    GPRReg callee;
    GPRReg newTarget;
    GPRReg count;
    GPRReg values;
};

struct NativeConstructorRegisters {
    GPRReg callee;
    GPRReg function;
    GPRReg globalObject;
    GPRReg scratch;
};

static void loadConstructorClassInfo(StubAssembler& jit, GPRReg callee, GPRReg result)
{
    loadStructure(jit, callee, result);
    jit.loadPtr(Address(result, Structure::classInfoOffset()), result);
}

static void loadNativeConstructor(StubAssembler& jit, ConstructSyntax syntax, const NativeConstructorRegisters& registers, JumpList& genericPath)
{
    const auto [callee, function, globalObject, scratch] = registers;
    RELEASE_ASSERT(noOverlap(callee, function, globalObject, scratch, instanceGPR));

    jit.load8(Address(callee, JSCell::typeInfoTypeOffset()), scratch);
    Jump isInternalFunction = jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(InternalFunctionType));
    genericPath.append(jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(JSFunctionType)));
    loadConstructorClassInfo(jit, callee, scratch);
    genericPath.append(jit.branchPtr(CCallHelpers::Equal, scratch, Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.boundFunctionClassInfo))));
    jit.loadPtr(Address(callee, JSFunction::offsetOfExecutableOrRareData()), function);
    Jump hasExecutable = jit.branchTestPtr(CCallHelpers::Zero, function, TrustedImm32(JSFunction::rareDataTag));
    jit.loadPtr(Address(function, FunctionRareData::offsetOfExecutable() - JSFunction::rareDataTag), function);
    hasExecutable.link(&jit);
    genericPath.append(jit.branchIfNotType(function, NativeExecutableType));
    jit.loadPtr(Address(function, NativeExecutable::offsetOfNativeFunctionFor(CodeSpecializationKind::CodeForConstruct)), function);
    jit.loadPtr(Address(callee, JSCallee::offsetOfScopeChain()), globalObject);
    Jump hasFunction = jit.jump();

    isInternalFunction.link(&jit);
    switch (syntax) {
    case ConstructSyntax::NewExpression:
        break;
    case ConstructSyntax::SuperCall:
        loadConstructorClassInfo(jit, callee, scratch);
        jit.loadRuntimePointer(Entry::ErrorConstructorClassInfo, function);
        genericPath.append(jit.branchPtr(CCallHelpers::Equal, scratch, function));
        break;
    }
    jit.loadPtr(Address(callee, InternalFunction::offsetOfNativeFunctionFor(CodeSpecializationKind::CodeForConstruct)), function);
    jit.loadPtr(Address(callee, InternalFunction::offsetOfGlobalObject()), globalObject);

    hasFunction.link(&jit);
    jit.loadRuntimePointer(Entry::NonConstructorHostFunction, scratch);
    genericPath.append(jit.branchPtr(CCallHelpers::Equal, function, scratch));
}

[[nodiscard]] static JumpList checkHostFrameCapacity(StubAssembler& jit, GPRReg count, GPRReg scratch)
{
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    JumpList overflowsStack;
    static_assert(stackAlignmentRegisters() == 2);
    jit.add32(TrustedImm32(CallFrame::headerSizeInRegisters + 1 + 1), count, scratch);
    jit.and32(TrustedImm32(~1), scratch);
    jit.lshiftPtr(TrustedImm32(3), scratch);
    jit.subPtr(sp, scratch, scratch);
    overflowsStack.append(jit.branchPtr(CCallHelpers::Above, Address(instanceGPR, AOT_INSTANCE_OFFSET(stackMirror) + StackManager::Mirror::offsetOfSoftStackLimit()), scratch));
    overflowsStack.append(jit.branchPtr(CCallHelpers::Above, scratch, GPRInfo::callFrameRegister));
    return overflowsStack;
}

static void callConstructorAndReturn(StubAssembler& jit, const HostCallFrame& frame, GPRReg function, GPRReg globalObject)
{
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    emitHostCallFrame(jit, frame);
    const GPRReg globalObjectArgument = jit.reuseScratch(StubRegister::A0);
    const GPRReg callFrameArgument = jit.reuseScratch(StubRegister::A1);
    const GPRReg scratch = jit.reuseScratch(StubRegister::T11);
    const GPRReg exception = jit.secondResult();
    RELEASE_ASSERT(noOverlap(function, globalObject, globalObjectArgument, callFrameArgument, scratch));
    jit.move(globalObject, globalObjectArgument);
    jit.move(sp, callFrameArgument);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), scratch);
    jit.storePtr(callFrameArgument, Address(scratch, VM::offsetOfTopCallFrame()));
    callNativeFunction(jit, CodeSpecializationKind::CodeForConstruct, function);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), exception);
    jit.loadPtr(Address(exception, VM::exceptionOffset()), exception);
    jit.emitFunctionEpilogue();
    jit.returnToCaller();
}

static void emitConstructFrontEnd(StubAssembler& jit, ConstructSyntax syntax, const ConstructOperands& operands, Entry rawOperation)
{
    const auto [callee, newTarget, count, values] = operands;
    const GPRReg function = jit.allocateScratch(StubRegister::T9);
    const GPRReg globalObject = jit.allocateScratch(globalObjectRegister);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T11);

    JumpList genericPath;
    genericPath.append(jit.branchIfNotCell(callee));
    genericPath.append(jit.branchIfNotCell(newTarget));
    loadNativeConstructor(jit, syntax, { callee, function, globalObject, scratch }, genericPath);

    jit.emitFunctionPrologue();
    JumpList overflowsStack = checkHostFrameCapacity(jit, count, scratch);
    callConstructorAndReturn(jit, { callee, newTarget, count, values }, function, globalObject);

    overflowsStack.link(&jit);
    jit.emitFunctionEpilogue();
    genericPath.link(&jit);
    jit.tailCallOperation(rawOperation, OperationCall::FirstArgument::Instance);
}

StubSignature signatureOfConstructFrontEnd(StubKey)
{
    return operationFrontEndSignature(4);
}

void generateConstructFrontEnd(StubAssembler& jit)
{
    const GPRReg callee = jit.clobberOperand(0);
    const GPRReg newTarget = jit.operand(1);
    const GPRReg count = jit.clobberOperand(2);
    const GPRReg values = jit.operand(3);
    emitConstructFrontEnd(jit, ConstructSyntax::NewExpression, { callee, newTarget, count, values }, Entry::RawConstruct);
}

StubSignature signatureOfSuperConstructFrontEnd(StubKey)
{
    return operationFrontEndSignature(4);
}

void generateSuperConstructFrontEnd(StubAssembler& jit)
{
    const GPRReg parent = jit.clobberOperand(0);
    const GPRReg newTarget = jit.operand(1);
    const GPRReg count = jit.clobberOperand(2);
    const GPRReg values = jit.operand(3);
    emitConstructFrontEnd(jit, ConstructSyntax::SuperCall, { parent, newTarget, count, values }, Entry::RawSuperConstruct);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
