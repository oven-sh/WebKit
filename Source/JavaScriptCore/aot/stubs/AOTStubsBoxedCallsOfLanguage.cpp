/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsBoxedCallsOfLanguage.h"

#if ENABLE(AOT)

#include "AOTBuiltinCells.h"
#include "AOTConvention.h"
#include "AOTInstance.h"
#include "AOTStubAssembler.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "AOTStubsBoxedCallsOfStrings.h"
#include "AOTStubsCallHost.h"
#include "AOTStubsStrings.h"
#include "JSCallee.h"
#include "JSString.h"
#include "JSType.h"
#include "JSTypeInfo.h"
#include "Structure.h"
#include <array>
#include <wtf/text/StringImpl.h>

namespace JSC { namespace AOT {

static void checkRealmOfCalleeInBoxedCallsOfLanguage(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    StubFamily family = jit.key().family();
    if (family == StubFamily::CallMethodOfBuiltinClass || family == StubFamily::TryCallMethodOfBuiltinClass || family == StubFamily::CallMethodThatMayBeUnread)
        return;
    JumpList isOfThisInstance;
    if (jit.key().family() == StubFamily::CallBoxedBuiltin)
        isOfThisInstance.append(jit.branch64(CCallHelpers::Below, calleeGPR, TrustedImm32(limitOfNumbersOfBuiltinCells)));
    jit.loadPtr(Address(calleeGPR, JSCallee::offsetOfScopeChain()), scratch);
    callsHostFunction.append(jit.branchPtr(CCallHelpers::NotEqual, scratch, Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject))));
    isOfThisInstance.link(&jit);
}

void emitStringIsWellFormed(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    jit.loadPtr(Address(thisGPR, JSString::offsetOfValue()), scratch);
    callsHostFunction.append(jit.branchIfRopeStringImpl(scratch));
    callsHostFunction.append(jit.branchTest32(CCallHelpers::Zero, Address(scratch, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.move(TrustedImm32(JSValue::ValueTrue), jit.result());
    jit.returnToCaller();
}

bool searchesFromPosition(unsigned count)
{
    return count == 2;
}

void emitStringSearchFromPosition(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    static_assert(argumentGPR(0) == operationArgumentGPR(0) && argumentGPR(1) == operationArgumentGPR(1) && noOverlap(thisGPR, calleeGPR, operationArgumentGPR(1), operationArgumentGPR(2), operationArgumentGPR(3)));
    static_assert(gprOf(StubRegister::A1) == operationArgumentGPR(1) && gprOf(StubRegister::A2) == operationArgumentGPR(2) && gprOf(StubRegister::A3) == operationArgumentGPR(3));
    const BoxedCallForm form = static_cast<BoxedCallForm>(jit.key().form());
    RELEASE_ASSERT(form == BoxedCallForm::StringIndexOf || form == BoxedCallForm::StringLastIndexOf || form == BoxedCallForm::StringStartsWith);

    const bool takesPosition = jit.key().count() > 1;
    if (!takesPosition && form == BoxedCallForm::StringLastIndexOf)
        return emitStringLastIndexOf(jit, scratch, callsHostFunction);

    callsHostFunction.append(jit.branchIfNotCell(argumentGPR(0)));
    callsHostFunction.append(jit.branchIfNotType(argumentGPR(0), StringType));
    if (!takesPosition) {
        const StubKey target = form == BoxedCallForm::StringIndexOf ? stringIndexOf() : stringStartsWith();
        checkStubOperands(target, { thisGPR, argumentGPR(0) });
        jit.jumpToStub(target);
        return;
    }

    RELEASE_ASSERT(searchesFromPosition(jit.key().count()));
    checkRealmOfCalleeInBoxedCallsOfLanguage(jit, scratch, callsHostFunction);
    callsHostFunction.append(jit.branchIfNotInt32(argumentGPR(1)));

    const GPRReg position = jit.reuseScratch(StubRegister::A3);
    jit.zeroExtend32ToWord(argumentGPR(1), position);
    if (form != BoxedCallForm::StringLastIndexOf) {
        const GPRReg length = scratch;
        jit.loadPtr(Address(thisGPR, JSString::offsetOfValue()), length);
        loadStringLength(jit, thisGPR, length, length);
        Jump isNotNegative = jit.branch32(CCallHelpers::GreaterThanOrEqual, position, TrustedImm32(0));
        jit.move(TrustedImm32(0), position);
        isNotNegative.link(&jit);
        Jump liesInString = jit.branch32(CCallHelpers::BelowOrEqual, position, length);
        jit.move(length, position);
        liesInString.link(&jit);
    }

    static_assert(areDistinctOnEveryISA(StubRegister::A0, StubRegister::A1, StubRegister::A2, StubRegister::A3, StubRegister::A5, StubRegister::T9, StubRegister::T10));
    static_assert(gprOf(StubRegister::T10) == calleeGPR);
    JumpList isNotThere;
    JumpList needsOperation;
    const GPRReg place = jit.reuseScratch(StubRegister::A1);
    if (form == BoxedCallForm::StringLastIndexOf)
        needsOperation.append(jit.branch32(CCallHelpers::GreaterThan, position, TrustedImm32(0)));
    else
        jit.move(position, place);
    branchUnlessTextStandsAtPlace(jit, { thisGPR, argumentGPR(0), place, jit.reuseScratch(StubRegister::A2), jit.reuseScratch(StubRegister::A5), scratch, jit.reuseScratch(StubRegister::T10) },
        form == BoxedCallForm::StringLastIndexOf ? PlaceOfSoughtText::IsStart : form == BoxedCallForm::StringIndexOf ? PlaceOfSoughtText::IsGivenAndLast : PlaceOfSoughtText::IsGiven, isNotThere, needsOperation);
    if (form == BoxedCallForm::StringStartsWith)
        jit.move(TrustedImm32(JSValue::ValueTrue), jit.result());
    else if (form == BoxedCallForm::StringLastIndexOf)
        jit.move(TrustedImm64(JSValue::encode(jsNumber(0))), jit.result());
    else
        boxInt32(jit, position, jit.result());
    jit.returnToCaller();

    isNotThere.link(&jit);
    if (form == BoxedCallForm::StringStartsWith)
        jit.move(TrustedImm32(JSValue::ValueFalse), jit.result());
    else
        jit.move(TrustedImm64(JSValue::encode(jsNumber(-1))), jit.result());
    jit.returnToCaller();

    needsOperation.link(&jit);
    jit.move(argumentGPR(0), jit.reuseScratch(StubRegister::A2));
    jit.move(thisGPR, jit.reuseScratch(StubRegister::A1));
    if (form == BoxedCallForm::StringStartsWith) {
        jit.callOperationAndReturn(Entry::operationAOTStringHasAt, { }, [&] {
            jit.or32(TrustedImm32(JSValue::ValueFalse), jit.result());
        });
        return;
    }
    jit.callOperationAndReturn(form == BoxedCallForm::StringIndexOf ? Entry::operationAOTStringIndexOfFrom : Entry::operationAOTStringLastIndexOf, { }, [&] {
        boxInt32(jit, jit.result(), jit.result());
    });
}

bool pushesSeveralValues(unsigned count)
{
    return count >= 2 && count <= BoxedCallEntry::maxArgumentCount;
}

void emitArrayPushOfSeveral(StubAssembler& jit)
{
    static constexpr std::array operands { StubRegister::A1, StubRegister::A2, StubRegister::A3, StubRegister::A4, StubRegister::A5 };
    static constexpr std::array operations { Entry::operationAOTArrayPushTwo, Entry::operationAOTArrayPushThree, Entry::operationAOTArrayPushFour };
    static_assert(BoxedCallEntry::maxArgumentCount + 1 == operands.size() && BoxedCallEntry::maxArgumentCount - 1 == operations.size() && BoxedCallEntry::maxArgumentCount <= numberOfArgumentGPRs);
    static_assert(gprOf(operands[0]) == operationArgumentGPR(1) && gprOf(operands[4]) == operationArgumentGPR(5) && argumentGPR(0) == operationArgumentGPR(0) && argumentGPR(3) == operationArgumentGPR(3));
    static_assert(noOverlap(thisGPR, operationArgumentGPR(1), operationArgumentGPR(2), operationArgumentGPR(3), operationArgumentGPR(4), operationArgumentGPR(5)));
    const unsigned count = jit.key().count();
    RELEASE_ASSERT(pushesSeveralValues(count));

    for (unsigned i = count; i--;)
        jit.move(argumentGPR(i), jit.reuseScratch(operands[i + 1]));
    jit.move(thisGPR, jit.reuseScratch(operands[0]));
    jit.jumpToCallOfOperation(operations[count - 2]);
}

void emitCallOfBooleanConstructor(StubAssembler& jit, GPRReg scratch)
{
    static_assert(JSValue::ValueTrue == (JSValue::ValueFalse | 1));
    static_assert(JSValue::ValueNull < JSValue::ValueUndefined && JSValue::ValueFalse < JSValue::ValueUndefined && JSValue::ValueTrue < JSValue::ValueUndefined);
    static_assert(JSValue::ValueTrue & 1 && !((JSValue::ValueFalse | JSValue::ValueNull | JSValue::ValueUndefined) & 1));
    constexpr GPRReg value = argumentGPR(0);
    const GPRReg classInfo = jit.allocateScratch(StubRegister::T9);
    RELEASE_ASSERT(noOverlap(scratch, classInfo, calleeGPR, thisGPR, instanceGPR, argumentGPR(0), argumentGPR(1), argumentGPR(2), argumentGPR(3)));
    JumpList isAnotherFunction;
    JumpList callsFunction;

    auto returnBoolean = [&](bool answer) {
        jit.move(TrustedImm32(answer ? JSValue::ValueTrue : JSValue::ValueFalse), jit.result());
        jit.returnToCaller();
    };
    auto returnWhetherIsNotZero = [&](GPRReg word) {
        jit.test32(CCallHelpers::NonZero, word, word, jit.result());
        jit.or32(TrustedImm32(JSValue::ValueFalse), jit.result());
        jit.returnToCaller();
    };

    loadStructure(jit, calleeGPR, scratch);
    jit.loadRuntimePointer(Entry::BooleanConstructorClassInfo, classInfo);
    isAnotherFunction.append(jit.branchPtr(CCallHelpers::NotEqual, Address(scratch, Structure::classInfoOffset()), classInfo));
    if (!jit.key().count()) {
        returnBoolean(false);
        isAnotherFunction.link(&jit);
        return;
    }

    Jump isCellOrNumber = jit.branch64(CCallHelpers::Above, value, TrustedImm32(JSValue::ValueUndefined));
    jit.and32(TrustedImm32(1), value, jit.result());
    jit.or32(TrustedImm32(JSValue::ValueFalse), jit.result());
    jit.returnToCaller();

    isCellOrNumber.link(&jit);
    Jump isCell = jit.branchIfCell(value);
    callsFunction.append(jit.branchIfNotInt32(value));
    returnWhetherIsNotZero(value);

    isCell.link(&jit);
    jit.load8(Address(value, JSCell::typeInfoTypeOffset()), scratch);
    Jump isNotObject = jit.branch32(CCallHelpers::Below, scratch, TrustedImm32(ObjectType));
    callsFunction.append(jit.branchTest8(CCallHelpers::NonZero, Address(value, JSCell::typeInfoFlagsOffset()), TrustedImm32(MasqueradesAsUndefined)));
    returnBoolean(true);

    isNotObject.link(&jit);
    callsFunction.append(jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(StringType)));
    jit.loadPtr(Address(value, JSString::offsetOfValue()), scratch);
    loadStringLength(jit, value, scratch, scratch);
    returnWhetherIsNotZero(scratch);

    jit.linkToStub(callsFunction, callHostFunction(jit.key().count()));
    isAnotherFunction.link(&jit);
}

void emitCallOfNumberOrStringConstructor(StubAssembler& jit, GPRReg scratch)
{
    constexpr GPRReg value = argumentGPR(0);
    const GPRReg classInfo = jit.reuseScratch(StubRegister::T9);
    RELEASE_ASSERT(noOverlap(scratch, classInfo, calleeGPR, thisGPR, instanceGPR, argumentGPR(0), argumentGPR(1), argumentGPR(2), argumentGPR(3)));
    const bool takesValue = jit.key().count() && jit.options().callsNumberOrStringByValue;
    JumpList callsFunction;

    loadStructure(jit, calleeGPR, scratch);
    jit.loadPtr(Address(scratch, Structure::classInfoOffset()), scratch);
    jit.loadRuntimePointer(Entry::NumberConstructorClassInfo, classInfo);
    Jump isNotNumberConstructor = jit.branchPtr(CCallHelpers::NotEqual, scratch, classInfo);
    if (!takesValue)
        callsFunction.append(jit.jump());
    else {
        Jump isNoNumber = jit.branchTest64(CCallHelpers::Zero, value, numberTag);
        jit.move(value, jit.result());
        jit.returnToCaller();
        isNoNumber.link(&jit);
        callsFunction.append(jit.branchIfNotCell(value));
        callsFunction.append(jit.branchIfNotType(value, StringType));
        jit.move(value, jit.reuseScratch(StubRegister::A1));
        jit.callOperationAndReturn(Entry::operationAOTNumberOfAnyValue, { });
    }

    isNotNumberConstructor.link(&jit);
    jit.loadRuntimePointer(Entry::StringConstructorClassInfo, classInfo);
    Jump isAnotherFunction = jit.branchPtr(CCallHelpers::NotEqual, scratch, classInfo);
    if (!takesValue)
        callsFunction.append(jit.jump());
    else {
        Jump isNumber = jit.branchTest64(CCallHelpers::NonZero, value, numberTag);
        callsFunction.append(jit.branchIfNotCell(value));
        callsFunction.append(jit.branchIfNotType(value, StringType));
        jit.move(value, jit.result());
        jit.returnToCaller();
        isNumber.link(&jit);
        jit.move(value, jit.reuseScratch(StubRegister::A1));
        jit.callOperationAndReturn(Entry::operationAOTStringOfAnyValue, { });
    }

    jit.linkToStub(callsFunction, callHostFunction(jit.key().count()));
    isAnotherFunction.link(&jit);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
