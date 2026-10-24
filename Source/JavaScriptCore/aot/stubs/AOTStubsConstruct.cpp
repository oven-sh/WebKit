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

static void loadConstructorClassInfo(StubAssembler& jit, GPRReg callee, GPRReg result)
{
    loadStructure(jit, callee, result);
    jit.loadPtr(Address(result, Structure::classInfoOffset()), result);
}

static void loadNativeConstructor(StubAssembler& jit, ConstructSyntax syntax, GPRReg callee, GPRReg function, GPRReg globalObject, GPRReg scratch, JumpList& genericPath)
{
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
    JumpList overflowsStack;
    static_assert(stackAlignmentRegisters() == 2);
    jit.add32(TrustedImm32(CallFrame::headerSizeInRegisters + 1 + 1), count, scratch);
    jit.and32(TrustedImm32(~1), scratch);
    jit.lshiftPtr(TrustedImm32(3), scratch);
    jit.subPtr(sp, scratch, scratch);
    overflowsStack.append(jit.branchPtr(CCallHelpers::Above, softStackLimit(), scratch));
    overflowsStack.append(jit.branchPtr(CCallHelpers::Above, scratch, fp));
    return overflowsStack;
}

static void callConstructorAndReturn(StubAssembler& jit, const HostCallFrame& frame, GPRReg function, GPRReg globalObject)
{
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

static void emitConstructFrontEnd(StubAssembler& jit, ConstructSyntax syntax, Entry rawOperation)
{
    const GPRReg callee = jit.clobberOperand(0);
    const GPRReg newTarget = jit.operand(1);
    const GPRReg count = jit.clobberOperand(2);
    const GPRReg values = jit.operand(3);
    const GPRReg function = jit.allocateScratch(StubRegister::T9);
    const GPRReg globalObject = jit.allocateScratch(globalObjectRegister);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T11);

    JumpList genericPath;
    genericPath.append(jit.branchIfNotCell(callee));
    genericPath.append(jit.branchIfNotCell(newTarget));
    loadNativeConstructor(jit, syntax, callee, function, globalObject, scratch, genericPath);

    jit.emitFunctionPrologue();
    JumpList overflowsStack = checkHostFrameCapacity(jit, count, scratch);
    callConstructorAndReturn(jit, { callee, newTarget, count, values }, function, globalObject);

    overflowsStack.link(&jit);
    jit.emitFunctionEpilogue();
    genericPath.link(&jit);
    jit.tailCallOperation(rawOperation, OperationCall::FirstArgument::Instance);
}

StubSignature signatureOfConstructFrontEnd(StubKey) { return operationFrontEndSignature(4); }
StubSignature signatureOfSuperConstructFrontEnd(StubKey) { return operationFrontEndSignature(4); }
void generateConstructFrontEnd(StubAssembler& jit) { emitConstructFrontEnd(jit, ConstructSyntax::NewExpression, Entry::RawConstruct); }
void generateSuperConstructFrontEnd(StubAssembler& jit) { emitConstructFrontEnd(jit, ConstructSyntax::SuperCall, Entry::RawSuperConstruct); }

// A function whose Structure is remembered has its `prototype` property at the remembered place, and an object found there is the answer.
static void loadRememberedPrototypeProperty(StubAssembler& jit, GPRReg function, GPRReg structureID, GPRReg place, GPRReg offset, GPRReg prototype, JumpList& isNotRemembered)
{
    static_assert(sizeof(PlaceOfPrototypeProperty) == sizeof(uint64_t) && !(numberOfPlacesOfPrototypeProperties & (numberOfPlacesOfPrototypeProperties - 1)));
    static_assert(sizeof(PrototypeOfClassObject) == 2 * sizeof(uint64_t) && !(numberOfPrototypesOfClassObjects & (numberOfPrototypesOfClassObjects - 1)));
    RELEASE_ASSERT(noOverlap(function, structureID, place, offset, prototype));
    jit.load32(Address(function, JSCell::structureIDOffset()), structureID);
    jit.move(structureID, place);
    jit.urshift32(TrustedImm32(shiftOfStructureIDForPlaceOfPrototypeProperty), place);
    jit.and32(TrustedImm32(numberOfPlacesOfPrototypeProperties - 1), place);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(classes.placesOfPrototypeProperties)), offset);
    jit.getEffectiveAddress(BaseIndex(offset, place, CCallHelpers::TimesEight), place);
    Jump isStructureNotRemembered = jit.branch32(CCallHelpers::NotEqual, Address(place, OBJECT_OFFSETOF(PlaceOfPrototypeProperty, structureID)), structureID);
    jit.load32(Address(place, OBJECT_OFFSETOF(PlaceOfPrototypeProperty, offset)), offset);
    jit.loadProperty(function, offset, prototype);
    isNotRemembered.append(jit.branchIfNotCell(prototype));
    isNotRemembered.append(jit.branchIfNotObject(prototype));
    Jump isLoaded = jit.jump();

    isStructureNotRemembered.link(&jit);
    jit.move(function, place);
    jit.urshift64(TrustedImm32(shiftOfClassObjectForPrototype), place);
    jit.and32(TrustedImm32(numberOfPrototypesOfClassObjects - 1), place);
    jit.lshift32(TrustedImm32(1), place);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(classes.prototypesOfClassObjects)), offset);
    jit.getEffectiveAddress(BaseIndex(offset, place, CCallHelpers::TimesEight), place);
    isNotRemembered.append(jit.branchPtr(CCallHelpers::NotEqual, Address(place, OBJECT_OFFSETOF(PrototypeOfClassObject, classObject)), function));
    jit.loadPtr(Address(place, OBJECT_OFFSETOF(PrototypeOfClassObject, prototype)), prototype);
    isLoaded.link(&jit);
}

// What the operation does first.
PreludeExit emitGetConstructPrototypePrelude(StubAssembler& jit)
{
    // A call that preserves registers has none to look with.
    if (jit.key().family() != StubFamily::CallOperation || jit.signature().result != StubResult::GPR)
        return PreludeExit::CallsOperation;

    constexpr GPRReg newTarget = operationArgumentGPR(1);
    const GPRReg structureID = jit.allocateScratch(StubRegister::A2);
    const GPRReg place = jit.allocateScratch(StubRegister::A3);
    const GPRReg offset = jit.allocateScratch(StubRegister::A4);
    const GPRReg prototype = jit.result();

    JumpList isNotRemembered;
    isNotRemembered.append(jit.branchIfNotCell(newTarget));
    loadRememberedPrototypeProperty(jit, newTarget, structureID, place, offset, prototype, isNotRemembered);
    jit.returnToCaller();

    isNotRemembered.link(&jit);
    return PreludeExit::CallsOperation;
}

// Where `super.name` begins to look: above the home object, which is the operand or the prototype property of the class object that is the operand.
PreludeExit emitGetSuperBasePrelude(StubAssembler& jit)
{
    // A call that preserves registers has none to look with. An operation that is called in tail position answers the caller of the stub in the register of its result.
    StubFamily family = jit.key().family();
    bool isTailCall = family == StubFamily::TailCallOperation;
    if (!isTailCall && (family != StubFamily::CallOperation || jit.signature().result != StubResult::GPR))
        return PreludeExit::CallsOperation;

    constexpr GPRReg home = operationArgumentGPR(1);
    constexpr GPRReg homeKind = operationArgumentGPR(2);
    const GPRReg structureID = jit.allocateScratch(StubRegister::A3);
    const GPRReg place = jit.allocateScratch(StubRegister::A4);
    const GPRReg offset = jit.allocateScratch(StubRegister::A5);
    const GPRReg homeObject = isTailCall ? jit.allocateScratch(StubRegister::R0) : jit.result();
    RELEASE_ASSERT(homeObject == GPRInfo::returnValueGPR && noOverlap(home, homeKind, structureID, place, offset, homeObject));
    static_assert(!static_cast<uint32_t>(Typed::HomeObjectKind::Object) && static_cast<uint32_t>(Typed::HomeObjectKind::ClassPrototype) == 1);

    JumpList callsOperation;
    callsOperation.append(jit.branchIfNotCell(home));
    jit.move(home, homeObject);
    Jump operandIsHomeObject = jit.branchTest32(CCallHelpers::Zero, homeKind);
    loadRememberedPrototypeProperty(jit, home, structureID, place, offset, homeObject, callsOperation);
    operandIsHomeObject.link(&jit);
    callsOperation.append(jit.branchTest8(CCallHelpers::NonZero, Address(homeObject, JSCell::typeInfoFlagsOffset()), TrustedImm32(OverridesGetPrototype)));
    loadStructure(jit, homeObject, homeObject);
    jit.load64(Address(homeObject, Structure::prototypeOffset()), homeObject);
    callsOperation.append(jit.branchTest64(CCallHelpers::Zero, homeObject));
    jit.returnToCaller();

    callsOperation.link(&jit);
    return PreludeExit::CallsOperation;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
