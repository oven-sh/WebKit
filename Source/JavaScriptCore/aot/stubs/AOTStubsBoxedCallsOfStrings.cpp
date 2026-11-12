/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsBoxedCallsOfStrings.h"

#if ENABLE(AOT)

#include "AOTBuiltinCells.h"
#include "AOTConvention.h"
#include "AOTInstance.h"
#include "AOTStubAssembler.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "AOTStubsStrings.h"
#include "JSCallee.h"
#include "JSString.h"
#include <limits>
#include <wtf/text/StringImpl.h>

namespace JSC { namespace AOT {

template<typename Forms>
static bool isFormOfStringAt(uint8_t form)
{
    if constexpr (requires { Forms::StringAt; })
        return form == static_cast<uint8_t>(Forms::StringAt);
    else
        return false;
}

template<typename Forms>
static bool isFormOfStringSubstr(uint8_t form)
{
    if constexpr (requires { Forms::StringSubstr; })
        return form == static_cast<uint8_t>(Forms::StringSubstr);
    else
        return false;
}

static void checkRealmOfCalleeOfString(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    StubFamily family = jit.key().family();
    if (family == StubFamily::CallMethodOfBuiltinClass || family == StubFamily::TryCallMethodOfBuiltinClass || family == StubFamily::CallMethodOfBuiltinClassWithSharedPattern || family == StubFamily::CallMethodThatMayBeUnread)
        return;
    JumpList isOfThisInstance;
    if (jit.key().family() == StubFamily::CallBoxedBuiltin)
        isOfThisInstance.append(jit.branch64(CCallHelpers::Below, calleeGPR, TrustedImm32(limitOfNumbersOfBuiltinCells)));
    jit.loadPtr(Address(calleeGPR, JSCallee::offsetOfScopeChain()), scratch);
    callsHostFunction.append(jit.branchPtr(CCallHelpers::NotEqual, scratch, Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject))));
    isOfThisInstance.link(&jit);
}

void emitStringCodeUnitAt(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    constexpr int32_t surrogateMask = 0xfc00;
    constexpr int32_t leadSurrogate = 0xd800;
    constexpr int32_t trailSurrogate = 0xdc00;
    constexpr int32_t surrogateOffset = (leadSurrogate << 10) + trailSurrogate - 0x10000;
    const bool readsCodePoint = jit.key().form() == static_cast<uint8_t>(BoxedCallForm::StringCodePointAt);
    const GPRReg impl = scratch;
    const GPRReg unit = jit.allocateScratch(StubRegister::T11);
    const GPRReg trail = jit.allocateScratch(StubRegister::T13);
    RELEASE_ASSERT(noOverlap(impl, unit, trail, calleeGPR, thisGPR, instanceGPR, argumentGPR(0)));

    callsHostFunction.append(jit.branchIfNotInt32(argumentGPR(0)));
    Label hasString = jit.label();
    jit.loadPtr(Address(thisGPR, JSString::offsetOfValue()), impl);
    Jump isRope = jit.branchIfRopeStringImpl(impl);
    JumpList isOutside;
    JumpList isNotSubstring;
    isOutside.append(jit.branch32(CCallHelpers::AboveOrEqual, argumentGPR(0), Address(impl, StringImpl::lengthMemoryOffset())));
    jit.zeroExtend32ToWord(argumentGPR(0), unit);
    Jump is16Bit = jit.branchTest32(CCallHelpers::Zero, Address(impl, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit()));
    jit.loadPtr(Address(impl, StringImpl::dataOffset()), impl);
    jit.load8(BaseIndex(impl, unit, CCallHelpers::TimesOne), unit);
    Label boxesUnit = jit.label();
    boxInt32(jit, unit, jit.result());
    jit.returnToCaller();

    isRope.link(&jit);
    loadSubstringCharacters(jit, thisGPR, impl, trail, unit, isNotSubstring);
    isOutside.append(jit.branch32(CCallHelpers::AboveOrEqual, argumentGPR(0), unit));
    jit.zeroExtend32ToWord(argumentGPR(0), unit);
    Jump pieceIs16Bit = jit.branchTest32(CCallHelpers::Zero, Address(impl, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit()));
    jit.load8(BaseIndex(trail, unit, CCallHelpers::TimesOne), unit);
    jit.jump().linkTo(boxesUnit, &jit);
    pieceIs16Bit.link(&jit);
    Jump hasSubstringPair;
    if (readsCodePoint) {
        const GPRReg unitsLeft = impl;
        jit.load32(Address(thisGPR, JSRopeString::offsetOfLength()), unitsLeft);
        jit.sub32(unit, unitsLeft);
        jit.getEffectiveAddress(BaseIndex(trail, unit, CCallHelpers::TimesTwo), trail);
        jit.load16(Address(trail), unit);
        jit.branch32(CCallHelpers::Equal, unitsLeft, TrustedImm32(1)).linkTo(boxesUnit, &jit);
        jit.load16(Address(trail, sizeof(char16_t)), trail);
        hasSubstringPair = jit.jump();
    } else {
        jit.load16(BaseIndex(trail, unit, CCallHelpers::TimesTwo), unit);
        jit.jump().linkTo(boxesUnit, &jit);
    }

    isOutside.link(&jit);
    jit.move(TrustedImm64(readsCodePoint ? JSValue::ValueUndefined : JSValue::encode(jsNaN())), jit.result());
    jit.returnToCaller();

    is16Bit.link(&jit);
    JumpList isOneUnit;
    if (readsCodePoint) {
        jit.add32(TrustedImm32(1), unit, trail);
        Jump isLastUnit = jit.branch32(CCallHelpers::AboveOrEqual, trail, Address(impl, StringImpl::lengthMemoryOffset()));
        jit.loadPtr(Address(impl, StringImpl::dataOffset()), impl);
        jit.load16(BaseIndex(impl, trail, CCallHelpers::TimesTwo), trail);
        jit.load16(BaseIndex(impl, unit, CCallHelpers::TimesTwo), unit);
        hasSubstringPair.link(&jit);
        jit.move(unit, impl);
        jit.and32(TrustedImm32(surrogateMask), impl);
        isOneUnit.append(jit.branch32(CCallHelpers::NotEqual, impl, TrustedImm32(leadSurrogate)));
        jit.move(trail, impl);
        jit.and32(TrustedImm32(surrogateMask), impl);
        isOneUnit.append(jit.branch32(CCallHelpers::NotEqual, impl, TrustedImm32(trailSurrogate)));
        jit.lshift32(TrustedImm32(10), unit);
        jit.add32(trail, unit);
        jit.sub32(TrustedImm32(surrogateOffset), unit);
        isOneUnit.append(jit.jump());
        isLastUnit.link(&jit);
    }
    jit.loadPtr(Address(impl, StringImpl::dataOffset()), impl);
    jit.load16(BaseIndex(impl, unit, CCallHelpers::TimesTwo), unit);
    isOneUnit.link(&jit);
    boxInt32(jit, unit, jit.result());
    jit.returnToCaller();

    isNotSubstring.link(&jit);
    flattenSubject(jit, thisGPR, { thisGPR, argumentGPR(0) }, hasString);
}

void emitStringLocaleCompare(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    const LocaleCompareRegisters registers {
        thisGPR, argumentGPR(0), jit.reuseScratch(StubRegister::A1), jit.reuseScratch(StubRegister::A2), jit.reuseScratch(StubRegister::A3), scratch,
        jit.allocateScratch(StubRegister::T11), jit.allocateScratch(StubRegister::T13)
    };
    RELEASE_ASSERT(noOverlap(registers.left, registers.right, registers.leftCharacters, registers.rightCharacters, registers.remaining, registers.leftUnit, registers.rightUnit, registers.weights, calleeGPR, instanceGPR));
    checkRealmOfCalleeOfString(jit, scratch, callsHostFunction);
    callsHostFunction.append(jit.branchIfNotCell(argumentGPR(0)));
    callsHostFunction.append(jit.branchIfNotType(argumentGPR(0), StringType));
    auto returnOrder = [&](int32_t order) {
        jit.move(TrustedImm64(JSValue::encode(jsNumber(order))), jit.result());
        jit.returnToCaller();
    };
    emitLocaleCompareOfASCII(jit, registers, ScopedLambda<void(int32_t)>(returnOrder), callsHostFunction);
}

void emitStringCharacterAt(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    static_assert(noOverlap(gprOf(StubRegister::A1), gprOf(StubRegister::A2), gprOf(StubRegister::A3), thisGPR, calleeGPR, argumentGPR(0)));
    const bool countsFromEnd = isFormOfStringAt<BoxedCallForm>(jit.key().form());
    const GPRReg length = scratch;
    checkRealmOfCalleeOfString(jit, scratch, callsHostFunction);
    callsHostFunction.append(jit.branchIfNotInt32(argumentGPR(0)));
    jit.loadPtr(Address(thisGPR, JSString::offsetOfValue()), length);
    loadStringLength(jit, thisGPR, length, length);

    const GPRReg string = jit.reuseScratch(StubRegister::A1);
    const GPRReg start = jit.reuseScratch(StubRegister::A2);
    const GPRReg end = jit.reuseScratch(StubRegister::A3);
    RELEASE_ASSERT(noOverlap(length, string, start, end, thisGPR, argumentGPR(0)));
    checkStubOperands(substring(), { string, start, end });
    jit.zeroExtend32ToWord(argumentGPR(0), start);
    if (countsFromEnd) {
        Jump countsFromStart = jit.branch32(CCallHelpers::GreaterThanOrEqual, argumentGPR(0), TrustedImm32(0));
        jit.add32(length, start);
        countsFromStart.link(&jit);
    }
    Jump isOutside = jit.branch32(CCallHelpers::AboveOrEqual, start, length);
    jit.add64(TrustedImm32(1), start, end);
    jit.move(thisGPR, string);
    jit.jumpToStub(substring());

    isOutside.link(&jit);
    if (countsFromEnd)
        jit.move(TrustedImm64(JSValue::ValueUndefined), jit.result());
    else
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(emptyString)), jit.result());
    jit.returnToCaller();
}

void emitStringPiece(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    const bool countsFromEnd = jit.key().form() != static_cast<uint8_t>(BoxedCallForm::StringSubstring);
    const bool takesLength = isFormOfStringSubstr<BoxedCallForm>(jit.key().form());
    static_assert(argumentGPR(1) == gprOf(StubRegister::A1) && noOverlap(gprOf(StubRegister::A2), gprOf(StubRegister::A3), thisGPR, calleeGPR, argumentGPR(0), argumentGPR(1)));
    const unsigned count = jit.key().count();

    checkRealmOfCalleeOfString(jit, scratch, callsHostFunction);
    callsHostFunction.append(jit.branchIfNotInt32(argumentGPR(0)));
    if (count == 2)
        callsHostFunction.append(jit.branchIfNotInt32(argumentGPR(1)));

    const GPRReg length = scratch;
    const GPRReg string = jit.reuseScratch(StubRegister::A1);
    const GPRReg start = jit.reuseScratch(StubRegister::A2);
    const GPRReg end = jit.reuseScratch(StubRegister::A3);
    RELEASE_ASSERT(noOverlap(length, string, start, end, thisGPR, argumentGPR(0)));
    checkStubOperands(substring(), { string, start, end });

    auto toPosition = [&](GPRReg relativeIndex, GPRReg position) {
        jit.signExtend32ToPtr(relativeIndex, position);
        JumpList isInside;
        Jump countsFromStart = jit.branch64(CCallHelpers::GreaterThanOrEqual, position, TrustedImm32(0));
        if (countsFromEnd) {
            jit.add64(length, position);
            isInside.append(jit.branch64(CCallHelpers::GreaterThanOrEqual, position, TrustedImm32(0)));
        }
        jit.move(TrustedImm32(0), position);
        isInside.append(jit.jump());
        countsFromStart.link(&jit);
        isInside.append(jit.branch64(CCallHelpers::LessThanOrEqual, position, length));
        jit.move(length, position);
        isInside.link(&jit);
    };
    jit.loadPtr(Address(thisGPR, JSString::offsetOfValue()), length);
    loadStringLength(jit, thisGPR, length, length);
    if (!takesLength) {
        JumpList isClamped;
        if (count == 2) {
            jit.zeroExtend32ToWord(argumentGPR(1), end);
            isClamped.append(jit.branch32(CCallHelpers::Above, end, length));
        } else
            jit.move(length, end);
        jit.zeroExtend32ToWord(argumentGPR(0), start);
        isClamped.append(jit.branch32(CCallHelpers::Above, start, end));
        jit.move(thisGPR, string);
        jit.jumpToStub(substring());
        isClamped.link(&jit);
    }
    if (count != 2)
        jit.move(length, end);
    else if (!takesLength)
        toPosition(argumentGPR(1), end);
    toPosition(argumentGPR(0), start);
    if (count == 2 && takesLength) {
        jit.signExtend32ToPtr(argumentGPR(1), end);
        Jump isNotNegative = jit.branch64(CCallHelpers::GreaterThanOrEqual, end, TrustedImm32(0));
        jit.move(TrustedImm32(0), end);
        isNotNegative.link(&jit);
        jit.add64(start, end);
        Jump endsInside = jit.branch64(CCallHelpers::LessThanOrEqual, end, length);
        jit.move(length, end);
        endsInside.link(&jit);
    }
    if (!countsFromEnd) {
        Jump isInOrder = jit.branch64(CCallHelpers::LessThanOrEqual, start, end);
        jit.move(start, length);
        jit.move(end, start);
        jit.move(length, end);
        isInOrder.link(&jit);
    }
    jit.move(thisGPR, string);
    jit.jumpToStub(substring());
}

void emitStringLastIndexOf(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    static_assert(argumentGPR(0) == operationArgumentGPR(0) && noOverlap(thisGPR, calleeGPR, operationArgumentGPR(1), operationArgumentGPR(2), operationArgumentGPR(3)));
    static_assert(gprOf(StubRegister::A1) == operationArgumentGPR(1) && gprOf(StubRegister::A2) == operationArgumentGPR(2) && gprOf(StubRegister::A3) == operationArgumentGPR(3));
    checkRealmOfCalleeOfString(jit, scratch, callsHostFunction);
    callsHostFunction.append(jit.branchIfNotCell(argumentGPR(0)));
    callsHostFunction.append(jit.branchIfNotType(argumentGPR(0), StringType));
    jit.move(argumentGPR(0), jit.reuseScratch(StubRegister::A2));
    jit.move(thisGPR, jit.reuseScratch(StubRegister::A1));
    jit.move(TrustedImm32(std::numeric_limits<int32_t>::max()), jit.reuseScratch(StubRegister::A3));
    jit.callOperationAndReturn(Entry::operationAOTStringLastIndexOf, { }, [&] {
        boxInt32(jit, jit.result(), jit.result());
    });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
