/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsStrings.h"

#if ENABLE(AOT)

#include "AOTLimits.h"
#include "AOTRecordTypes.h"
#include "AOTRuntimeTable.h"
#include "AOTSlotRepresentation.h"
#include "AOTStringBuilderStorage.h"
#include "AOTStringBuilderWords.h"
#include "AOTStubAssembler.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "AOTThreshold.h"
#include "JSString.h"
#include <unicode/utf16.h>
#include <wtf/text/StringImpl.h>

namespace JSC { namespace AOT {

using Saves = OperationCall::Saves;

static constexpr int32_t maxLengthComparedWithoutCall = defaultLimitValue(LimitID::MaxInlineStringCompareLength, hostArchitecture());
AOT_THRESHOLD(int32_t, maxLengthSearchedByBytes, 32);

static StubSignature signatureOfCharactersInPlace()
{
    StubSignature signature = StubSignature::withGPRResult({ gprOf(StubRegister::T12) });
    signature.firstResult = gprOf(StubRegister::T12);
    return signature;
}

StubSignature signatureOfRopeCharacters(StubKey) { return signatureOfCharactersInPlace(); }
StubSignature signatureOfFlattenString(StubKey) { return signatureOfCharactersInPlace(); }
StubSignature signatureOfStringsAreEqual(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::T11), gprOf(StubRegister::T12) }); }
StubSignature signatureOfDistinctStringsAreEqual(StubKey key) { return signatureOfStringsAreEqual(key); }
StubSignature signatureOfCompareStrings(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::T11), gprOf(StubRegister::T12) }); }
StubSignature signatureOfStringCodePointAt(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2) }); }
StubSignature signatureOfFindCharacter8(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::A0), gprOf(StubRegister::A1), gprOf(StubRegister::A2), gprOf(StubRegister::A3) }); }
StubSignature signatureOfStringBuilderAppendString(StubKey) { return StubSignature::withoutResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2) }); }
StubSignature signatureOfStringBuilderDiscard(StubKey) { return StubSignature::withoutResult({ gprOf(StubRegister::A1) }); }
StubSignature signatureOfStackStringBuilderAppendString(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2), gprOf(StubRegister::A3) }); }

namespace {

// No move reaches beyond the characters: the last one of each length ends where they end, and lies over the one in front of it where it has to.
struct CharacterCopy {
    NEVER_INLINE void moveTwice(unsigned width) const
    {
        const BaseIndex lastOfSource(source, length, CCallHelpers::TimesOne, -static_cast<int32_t>(width));
        const BaseIndex lastOfTarget(target, length, CCallHelpers::TimesOne, -static_cast<int32_t>(width));
        switch (width) {
        case 8:
            jit.load64(Address(source), first);
            jit.load64(lastOfSource, second);
            jit.store64(first, Address(target));
            jit.store64(second, lastOfTarget);
            return;
        case 4:
            jit.load32(Address(source), first);
            jit.load32(lastOfSource, second);
            jit.store32(first, Address(target));
            jit.store32(second, lastOfTarget);
            return;
        case 2:
            jit.load16(Address(source), first);
            jit.load16(lastOfSource, second);
            jit.store16(first, Address(target));
            jit.store16(second, lastOfTarget);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    NEVER_INLINE void moveSixteenAt(GPRReg index, int32_t displacement) const
    {
        jit.load64(BaseIndex(source, index, CCallHelpers::TimesOne, displacement), first);
        jit.load64(BaseIndex(source, index, CCallHelpers::TimesOne, displacement + 8), second);
        jit.store64(first, BaseIndex(target, index, CCallHelpers::TimesOne, displacement));
        jit.store64(second, BaseIndex(target, index, CCallHelpers::TimesOne, displacement + 8));
    }

    // Without a list of jumps, returns to the caller of the stub.
    NEVER_INLINE void copy(JumpList* isCopied) const
    {
        auto leave = [&] {
            if (isCopied)
                isCopied->append(jit.jump());
            else
                jit.returnToCaller();
        };
        Jump isLong = jit.branch32(CCallHelpers::Above, length, TrustedImm32(16));
        for (unsigned width : { 8u, 4u, 2u }) {
            Jump isShorter = jit.branch32(CCallHelpers::Below, length, TrustedImm32(width));
            moveTwice(width);
            leave();
            isShorter.link(&jit);
        }
        Jump isEmpty = jit.branchTest32(CCallHelpers::Zero, length);
        jit.load8(Address(source), first);
        jit.store8(first, Address(target));
        if (isCopied)
            isCopied->append(isEmpty);
        else
            isEmpty.link(&jit);
        leave();

        isLong.link(&jit);
        jit.move(TrustedImm32(0), offset);
        Label loop = jit.label();
        moveSixteenAt(offset, 0);
        jit.add32(TrustedImm32(16), offset);
        jit.add32(TrustedImm32(16), offset, first);
        jit.branch32(CCallHelpers::BelowOrEqual, first, length).linkTo(loop, &jit);
        moveSixteenAt(length, -16);
        if (!isCopied)
            jit.returnToCaller();
    }

    // The source holds the first word of the rope. The target ends behind the last piece. Whoever meets a piece that is itself a rope leaves by the slow path.
    NEVER_INLINE void copyPiecesOfRope(GPRReg rope, JumpList& slowPath) const
    {
        auto copyPiece = [&] {
            JumpList isCopied;
            jit.loadPtr(Address(source, JSString::offsetOfValue()), source);
            slowPath.append(jit.branchIfRopeStringImpl(source));
            jit.load32(Address(source, StringImpl::lengthMemoryOffset()), length);
            jit.loadPtr(Address(source, StringImpl::dataOffset()), source);
            copy(&isCopied);
            isCopied.link(&jit);
            jit.add64(length, target);
        };
        jit.and64(TrustedImm32(static_cast<int32_t>(JSRopeString::stringMask)), source);
        copyPiece();
        loadSubstringBaseAndOffset(jit, rope, source, offset);
        copyPiece();
        jit.load64(Address(rope, JSRopeString::offsetOfFiber2()), source);
        jit.urshift64(TrustedImm32(JSRopeString::CompactFibers::shiftOfFiber2InUpperWord), source);
        Jump hasTwoPieces = jit.branchTestPtr(CCallHelpers::Zero, source);
        copyPiece();
        hasTwoPieces.link(&jit);
    }

    StubAssembler& jit;
    GPRReg source;
    GPRReg target;
    GPRReg length;
    GPRReg first;
    GPRReg second;
    GPRReg offset;
};

} // anonymous namespace

// A flat string of 8-bit characters that is short enough to be copied and fits into the room that the buffer has left. Everything else is the operation's.
void generateStringBuilderAppendString(StubAssembler& jit)
{
    using Storage = StringBuilderStorage;
    static_assert(gprOf(StubRegister::A1) == operationArgumentGPR(1) && gprOf(StubRegister::A2) == operationArgumentGPR(2));
    const GPRReg builder = jit.operand(0);
    const GPRReg string = jit.operand(1);
    const GPRReg source = jit.allocateScratch(StubRegister::T10);
    const GPRReg length = jit.allocateScratch(StubRegister::T11);
    const GPRReg target = jit.allocateScratch(StubRegister::T12);
    const GPRReg first = jit.allocateScratch(StubRegister::T14);
    const GPRReg second = jit.allocateScratch(firstStubOperandRegister);
    const GPRReg offset = jit.allocateScratch(StubRegister::T9);
    RELEASE_ASSERT(noOverlap(builder, string, offset, source, length, target, first, second));
    const CharacterCopy characters { jit, source, target, length, first, second, offset };
    JumpList slowPath;

    jit.loadPtr(Address(string, JSString::offsetOfValue()), source);
    Jump isRope = jit.branchIfRopeStringImpl(source);
    slowPath.append(jit.branchTest32(CCallHelpers::Zero, Address(source, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.load32(Address(source, StringImpl::lengthMemoryOffset()), length);
    slowPath.append(jit.branch32(CCallHelpers::AboveOrEqual, length, Address(builder, Storage::offsetOfMinLengthAppendedByReference())));
    jit.load32(Address(builder, Storage::offsetOfPosition()), target);
    Jump hasText = jit.branchTest32(CCallHelpers::NonZero, target);
    slowPath.append(jit.branch32(CCallHelpers::AboveOrEqual, length, Address(builder, StringBuilderStorage::offsetOfMinLengthAdoptedByFinish())));
    hasText.link(&jit);
    jit.add64(length, target, first);
    jit.load32(Address(builder, Storage::offsetOfLimit()), second);
    slowPath.append(jit.branch64(CCallHelpers::Above, first, second));
    jit.store32(first, Address(builder, Storage::offsetOfPosition()));
    jit.addPtr(Address(builder, Storage::offsetOfCharacters()), target);
    jit.loadPtr(Address(source, StringImpl::dataOffset()), source);
    characters.copy(nullptr);

    // A short rope of flat 8-bit strings, as `first + " " + last` kept in a field is: its pieces are copied one after the other.
    isRope.link(&jit);
    slowPath.append(jit.branchTestPtr(CCallHelpers::NonZero, source, TrustedImm32(JSRopeString::isSubstringInPointer)));
    slowPath.append(jit.branchTestPtr(CCallHelpers::Zero, source, TrustedImm32(JSRopeString::is8BitInPointer)));
    jit.load32(Address(string, JSRopeString::offsetOfLength()), length);
    slowPath.append(jit.branch32(CCallHelpers::AboveOrEqual, length, Address(builder, Storage::offsetOfMinLengthAppendedByReference())));
    jit.load32(Address(builder, Storage::offsetOfPosition()), target);
    slowPath.append(jit.branchTest32(CCallHelpers::Zero, target));
    jit.add64(length, target, first);
    jit.load32(Address(builder, Storage::offsetOfLimit()), second);
    slowPath.append(jit.branch64(CCallHelpers::Above, first, second));
    jit.addPtr(Address(builder, Storage::offsetOfCharacters()), target);
    characters.copyPiecesOfRope(string, slowPath);
    jit.load32(Address(string, JSRopeString::offsetOfLength()), length);
    jit.add32(length, Address(builder, Storage::offsetOfPosition()));
    jit.returnToCaller();

    slowPath.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTStringBuilderAppendString, { .returns = OperationCall::Returns::Void, .saves = Saves::CallerSavedRegisters });
}

// A builder without text is as it came out of the pool, and its link to the next one has been null since. Into an empty pool it goes by one store, which is into no cell.
void generateStringBuilderDiscard(StubAssembler& jit)
{
    static_assert(gprOf(StubRegister::A1) == operationArgumentGPR(1));
    const GPRReg builder = jit.operand(0);
    const Address pool(instanceGPR, AOT_INSTANCE_OFFSET(stringBuilder.freeStorage));
    JumpList slowPath;

    slowPath.append(jit.branchTestPtr(CCallHelpers::NonZero, pool));
    slowPath.append(jit.branchTest32(CCallHelpers::NonZero, Address(builder, StringBuilderStorage::offsetOfPosition())));
    slowPath.append(jit.branchTest32(CCallHelpers::NonZero, Address(builder, StringBuilderStorage::offsetOfLengthOutsideBuffer())));
    jit.storePtr(builder, pool);
    jit.returnToCaller();

    slowPath.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTStringBuilderDiscard, { .returns = OperationCall::Returns::Void, .saves = Saves::CallerSavedRegisters, .checksException = false });
}

// The same two kinds of strings as above, and a piece of a flat string. The cursor is left alone, so the slow path starts with nothing done.
void generateStackStringBuilderAppendString(StubAssembler& jit)
{
    static_assert(gprOf(StubRegister::A1) == operationArgumentGPR(1) && gprOf(StubRegister::A2) == operationArgumentGPR(2) && gprOf(StubRegister::A3) == operationArgumentGPR(3));
    const GPRReg builder = jit.operand(0);
    const GPRReg cursor = jit.operand(1);
    const GPRReg string = jit.operand(2);
    const GPRReg end = jit.result();
    const GPRReg source = jit.allocateScratch(StubRegister::T10);
    const GPRReg length = jit.allocateScratch(StubRegister::T11);
    const GPRReg first = jit.allocateScratch(StubRegister::T12);
    const GPRReg second = jit.allocateScratch(StubRegister::T9);
    const GPRReg offset = jit.allocateScratch(StubRegister::T13);
    RELEASE_ASSERT(noOverlap(builder, cursor, string, end, source, length, first, second, offset));
    const Address limit(builder, OBJECT_OFFSETOF(StackStringBuilder, limit));
    JumpList slowPath;

    jit.loadPtr(Address(string, JSString::offsetOfValue()), source);
    Jump isRope = jit.branchIfRopeStringImpl(source);
    slowPath.append(jit.branchTest32(CCallHelpers::Zero, Address(source, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.load32(Address(source, StringImpl::lengthMemoryOffset()), length);
    slowPath.append(jit.branch32(CCallHelpers::AboveOrEqual, length, TrustedImm32(StackStringBuilder::minLengthAppendedByReference)));
    jit.add64(length, cursor, end);
    slowPath.append(jit.branchPtr(CCallHelpers::Above, end, limit));
    jit.loadPtr(Address(source, StringImpl::dataOffset()), source);
    Label copy = jit.label();
    CharacterCopy { jit, source, cursor, length, first, second, offset }.copy(nullptr);

    isRope.link(&jit);
    slowPath.append(jit.branchTestPtr(CCallHelpers::Zero, source, TrustedImm32(JSRopeString::is8BitInPointer)));
    jit.load32(Address(string, JSRopeString::offsetOfLength()), length);
    slowPath.append(jit.branch32(CCallHelpers::AboveOrEqual, length, TrustedImm32(StackStringBuilder::minLengthAppendedByReference)));
    jit.add64(length, cursor, end);
    slowPath.append(jit.branchPtr(CCallHelpers::Above, end, limit));
    Jump hasPieces = jit.branchTestPtr(CCallHelpers::Zero, source, TrustedImm32(JSRopeString::isSubstringInPointer));
    loadSubstringBaseAndOffset(jit, string, source, offset);
    jit.loadPtr(Address(source, JSString::offsetOfValue()), source);
    jit.loadPtr(Address(source, StringImpl::dataOffset()), source);
    jit.addPtr(offset, source);
    jit.jump().linkTo(copy, &jit);

    hasPieces.link(&jit);
    jit.move(cursor, end);
    CharacterCopy { jit, source, end, length, first, second, offset }.copyPiecesOfRope(string, slowPath);
    jit.returnToCaller();

    slowPath.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTStackStringBuilderAppend, { .saves = Saves::CallerSavedRegisters });
}

PreludeExit emitJSONAppendValuePrelude(StubAssembler& jit)
{
    if (jit.key().family() != StubFamily::CallOperation || jit.signature().result != StubResult::TwoGPRs)
        return PreludeExit::CallsOperation;

    constexpr uint64_t reciprocalOfTen = 0xcccccccd;
    constexpr unsigned shiftOfReciprocalOfTen = 35;
    constexpr auto fourBytes = [](const char (&text)[5]) {
        return static_cast<int32_t>(text[0] | text[1] << 8 | text[2] << 16 | text[3] << 24);
    };
    const GPRReg builder = jit.allocateScratch(StubRegister::A1);
    const GPRReg cursor = jit.allocateScratch(StubRegister::A2);
    const GPRReg value = jit.allocateScratch(StubRegister::A3);
    const GPRReg reciprocal = jit.allocateScratch(StubRegister::A4);
    const GPRReg quotient = jit.allocateScratch(StubRegister::A5);
    const GPRReg end = jit.result();
    const GPRReg place = builder;
    const GPRReg product = cursor;
    static_assert(gprOf(StubRegister::A1) == operationArgumentGPR(1) && gprOf(StubRegister::A2) == operationArgumentGPR(2) && gprOf(StubRegister::A3) == operationArgumentGPR(3));
    RELEASE_ASSERT(noOverlap(builder, cursor, value, reciprocal, quotient, end));
    JumpList callsOperation;
    auto returnEnd = [&] {
        jit.move(TrustedImm32(static_cast<int32_t>(JSONOutcome::Parsed)), jit.secondResult());
        jit.returnToCaller();
    };
    auto returnBehind = [&](int32_t length) {
        jit.addPtr(TrustedImm32(length), cursor, end);
        returnEnd();
    };

    callsOperation.append(jit.branchIfCell(value));
    jit.load32(Address(builder, StringBuilderStorage::offsetOfLimit()), end);
    jit.addPtr(Address(builder, StringBuilderStorage::offsetOfCharacters()), end);
    jit.subPtr(cursor, end);
    callsOperation.append(jit.branch64(CCallHelpers::Below, end, TrustedImm32(jsonNumberBytes + maxStringBuilderReservation)));
    Jump isNotInt32 = jit.branch64(CCallHelpers::Below, value, numberTag);
    callsOperation.append(jit.branch32(CCallHelpers::LessThan, value, TrustedImm32(0)));

    jit.zeroExtend32ToWord(value, value);
    jit.move(TrustedImm64(reciprocalOfTen), reciprocal);
    jit.move(value, quotient);
    jit.addPtr(TrustedImm32(1), cursor, end);
    Label countsDigit = jit.label();
    Jump isLastDigit = jit.branch32(CCallHelpers::Below, quotient, TrustedImm32(10));
    jit.mul64(reciprocal, quotient);
    jit.urshift64(TrustedImm32(shiftOfReciprocalOfTen), quotient);
    jit.addPtr(TrustedImm32(1), end);
    jit.jump().linkTo(countsDigit, &jit);

    isLastDigit.link(&jit);
    jit.move(end, place);
    Label writesDigit = jit.label();
    jit.move(value, quotient);
    jit.mul64(reciprocal, quotient);
    jit.urshift64(TrustedImm32(shiftOfReciprocalOfTen), quotient);
    jit.lshift32(quotient, TrustedImm32(2), product);
    jit.add32(quotient, product);
    jit.lshift32(TrustedImm32(1), product);
    jit.sub32(product, value);
    jit.add32(TrustedImm32('0'), value);
    jit.subPtr(TrustedImm32(1), place);
    jit.store8(value, Address(place));
    jit.move(quotient, value);
    jit.branchTest32(CCallHelpers::NonZero, value).linkTo(writesDigit, &jit);
    returnEnd();

    isNotInt32.link(&jit);
    Jump isNotTrue = jit.branch64(CCallHelpers::NotEqual, value, TrustedImm32(JSValue::ValueTrue));
    jit.store32(TrustedImm32(fourBytes("true")), Address(cursor));
    returnBehind(4);

    isNotTrue.link(&jit);
    Jump isNotFalse = jit.branch64(CCallHelpers::NotEqual, value, TrustedImm32(JSValue::ValueFalse));
    jit.store32(TrustedImm32(fourBytes("fals")), Address(cursor));
    jit.store8(TrustedImm32('e'), Address(cursor, 4));
    returnBehind(5);

    isNotFalse.link(&jit);
    callsOperation.append(jit.branch64(CCallHelpers::NotEqual, value, TrustedImm32(JSValue::ValueNull)));
    jit.store32(TrustedImm32(fourBytes("null")), Address(cursor));
    returnBehind(4);

    callsOperation.link(&jit);
    return PreludeExit::CallsOperation;
}

void generateRopeCharacters(StubAssembler& jit)
{
    const GPRReg string = jit.clobberOperand(0);
    const GPRReg data = jit.result();
    const GPRReg fiber = jit.allocateScratch(StubRegister::T9);
    const GPRReg offset = jit.allocateScratch(StubRegister::T13);
    RELEASE_ASSERT(string == data);
    auto loadImplCharacters = [&] {
        jit.loadPtr(Address(fiber, StringImpl::dataOffset()), data);
    };
    jit.loadPtr(Address(string, JSString::offsetOfValue()), fiber);
    Jump isFlat = jit.branchTestPtr(CCallHelpers::Zero, fiber, TrustedImm32(JSString::isRopeInPointer));
    Jump notSubstring = jit.branchTestPtr(CCallHelpers::Zero, fiber, TrustedImm32(JSRopeString::isSubstringInPointer));
    loadSubstringBaseAndOffset(jit, string, fiber, offset);
    jit.loadPtr(Address(fiber, JSString::offsetOfValue()), fiber);
    loadImplCharacters();
    Jump is8Bit = jit.branchTest32(CCallHelpers::NonZero, Address(fiber, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit()));
    jit.lshift64(TrustedImm32(1), offset);
    is8Bit.link(&jit);
    jit.addPtr(offset, data);
    jit.returnToCaller();

    isFlat.link(&jit);
    loadImplCharacters();
    jit.returnToCaller();

    notSubstring.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTFlatten, { .saves = Saves::CallerSavedRegisters, .secondArgumentIn = string, .valueDestination = fiber }, loadImplCharacters);
}

void generateFlattenString(StubAssembler& jit)
{
    const GPRReg string = jit.operand(0);
    const GPRReg fiber = jit.allocateScratch(StubRegister::T9);
    RELEASE_ASSERT(string == jit.result());

    jit.loadPtr(Address(string, JSString::offsetOfValue()), fiber);
    Jump isRope = jit.branchIfRopeStringImpl(fiber);
    jit.returnToCaller();

    isRope.link(&jit);
    // The returned impl is unused. Sending it to the scratch register makes the call restore every other register, including the cell's.
    jit.callOperationAndReturn(Entry::operationAOTFlatten, { .saves = Saves::CallerSavedRegisters, .secondArgumentIn = string, .valueDestination = fiber });
}

void generateStringCodePointAt(StubAssembler& jit)
{
    const GPRReg string = jit.operand(0);
    const GPRReg position = jit.operand(1);
    const GPRReg codePoint = jit.result();
    const GPRReg impl = jit.allocateScratch(StubRegister::T9);
    const GPRReg length = jit.allocateScratch(StubRegister::T11);
    const GPRReg trail = jit.allocateScratch(StubRegister::T10);
    const GPRReg characters = impl;
    const GPRReg surrogateBits = length;
    RELEASE_ASSERT(noOverlap(string, position, codePoint, impl, length, trail));
    const TrustedImm32 surrogateMask(~0x3ff);

    auto readFromImpl = [&] {
        JumpList done;
        jit.move(TrustedImm64(undefinedInteger), codePoint);
        jit.load32(Address(impl, StringImpl::lengthMemoryOffset()), length);
        done.append(jit.branch64(CCallHelpers::AboveOrEqual, position, length));
        Jump is16Bit = jit.branchTest32(CCallHelpers::Zero, Address(impl, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit()));
        jit.loadPtr(Address(impl, StringImpl::dataOffset()), characters);
        jit.load8(BaseIndex(characters, position, CCallHelpers::TimesOne), codePoint);
        done.append(jit.jump());

        is16Bit.link(&jit);
        jit.loadPtr(Address(impl, StringImpl::dataOffset()), characters);
        jit.load16(BaseIndex(characters, position, CCallHelpers::TimesTwo), codePoint);
        jit.and32(surrogateMask, codePoint, trail);
        done.append(jit.branch32(CCallHelpers::NotEqual, trail, TrustedImm32(0xd800)));
        jit.sub32(TrustedImm32(1), length);
        done.append(jit.branch64(CCallHelpers::AboveOrEqual, position, length));
        jit.load16(BaseIndex(characters, position, CCallHelpers::TimesTwo, sizeof(char16_t)), trail);
        jit.and32(surrogateMask, trail, surrogateBits);
        done.append(jit.branch32(CCallHelpers::NotEqual, surrogateBits, TrustedImm32(0xdc00)));
        jit.lshift32(TrustedImm32(10), codePoint);
        jit.add32(trail, codePoint);
        jit.sub32(TrustedImm32(U16_SURROGATE_OFFSET), codePoint);
        done.link(&jit);
    };

    jit.loadPtr(Address(string, JSString::offsetOfValue()), impl);
    Jump isRope = jit.branchIfRopeStringImpl(impl);
    readFromImpl();
    jit.returnToCaller();

    isRope.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTFlatten, { .saves = Saves::CallerSavedRegisters, .secondArgumentIn = string, .valueDestination = impl }, readFromImpl);
}

void loadStringLength(StubAssembler& jit, GPRReg string, GPRReg fiber, GPRReg length)
{
    Jump isRope = jit.branchIfRopeStringImpl(fiber);
    jit.load32(Address(fiber, StringImpl::lengthMemoryOffset()), length);
    Jump isLoaded = jit.jump();
    isRope.link(&jit);
    jit.load32(Address(string, JSRopeString::offsetOfLength()), length);
    isLoaded.link(&jit);
}

static void compareImplsAndReturn(StubAssembler& jit, Entry leaf, GPRReg firstImpl, GPRReg secondImpl)
{
    static_assert(leafArgumentGPR(0) == gprOf(StubRegister::A0) && leafArgumentGPR(1) == gprOf(StubRegister::A1));
    // On x86-64 the second operand arrives in the register of the leaf's first argument.
    if (jit.operand(1) == leafArgumentGPR(0))
        jit.clobberOperand(1);
    const GPRReg firstArgument = jit.reuseScratch(StubRegister::A0);
    const GPRReg secondArgument = jit.reuseScratch(StubRegister::A1);
    RELEASE_ASSERT(jit.result() == GPRInfo::returnValueGPR);
    RELEASE_ASSERT(noOverlap(firstImpl, secondImpl, firstArgument, secondArgument));
    jit.move(firstImpl, firstArgument);
    jit.move(secondImpl, secondArgument);
    jit.callLeaf(leaf);
    jit.returnToCaller();
}

void branchIfCharactersDiffer(StubAssembler& jit, const ComparedImpls& impls, JumpList& differ, JumpList& needsLeaf)
{
    auto [first, second, length, flagsOfBoth, word] = impls;
    RELEASE_ASSERT(noOverlap(first, second, length, flagsOfBoth, word));

    Jump isLong = jit.branch32(CCallHelpers::Above, length, TrustedImm32(maxLengthComparedWithoutCall));
    auto countBytes = [&] {
        Jump areBoth8Bit = jit.branchTest32(CCallHelpers::NonZero, flagsOfBoth, TrustedImm32(StringImpl::flagIs8Bit()));
        needsLeaf.append(jit.branchTest32(CCallHelpers::NonZero, Address(first, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
        needsLeaf.append(jit.branchTest32(CCallHelpers::NonZero, Address(second, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
        jit.lshift32(TrustedImm32(1), length);
        areBoth8Bit.link(&jit);
    };
    countBytes();
    jit.loadPtr(Address(first, StringImpl::dataOffset()), first);
    jit.loadPtr(Address(second, StringImpl::dataOffset()), second);
    branchIfBytesDiffer(jit, { first, second, length, flagsOfBoth, word }, differ, DifferingWords::MayBeUnloaded);
    Jump areSame = jit.jump();

    isLong.link(&jit);
    static_assert(maxLengthComparedWithoutCall >= static_cast<int32_t>(sizeof(uint64_t)));
    countBytes();
    const GPRReg otherWord = flagsOfBoth;
    jit.loadPtr(Address(first, StringImpl::dataOffset()), word);
    jit.load64(Address(word), word);
    jit.loadPtr(Address(second, StringImpl::dataOffset()), otherWord);
    jit.load64(Address(otherWord), otherWord);
    differ.append(jit.branch64(CCallHelpers::NotEqual, word, otherWord));
    jit.loadPtr(Address(first, StringImpl::dataOffset()), word);
    jit.load64(BaseIndex(word, length, CCallHelpers::TimesOne, -static_cast<int32_t>(sizeof(uint64_t))), word);
    jit.loadPtr(Address(second, StringImpl::dataOffset()), otherWord);
    jit.load64(BaseIndex(otherWord, length, CCallHelpers::TimesOne, -static_cast<int32_t>(sizeof(uint64_t))), otherWord);
    differ.append(jit.branch64(CCallHelpers::NotEqual, word, otherWord));
    needsLeaf.append(jit.jump());

    areSame.link(&jit);
}

void branchIfCharactersInPlaceDiffer(StubAssembler& jit, const ComparedStrings& strings, JumpList& differ, JumpList& needsOperation)
{
    auto [first, second, firstFiber, secondFiber, length, offset, word] = strings;
    needsOperation.append(jit.branch32(CCallHelpers::Above, length, TrustedImm32(maxLengthComparedWithoutCall)));
    for (auto [string, fiber] : { std::pair { first, firstFiber }, std::pair { second, secondFiber } }) {
        Jump isRope = jit.branchIfRopeStringImpl(fiber);
        needsOperation.append(jit.branchTest32(CCallHelpers::Zero, Address(fiber, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
        jit.loadPtr(Address(fiber, StringImpl::dataOffset()), fiber);
        Jump hasCharacters = jit.jump();
        isRope.link(&jit);
        load8BitCharactersOfSubstring(jit, string, fiber, offset, needsOperation);
        hasCharacters.link(&jit);
    }
    branchIfBytesDiffer(jit, { firstFiber, secondFiber, length, offset, word }, differ, DifferingWords::MayBeUnloaded);
}

void generateStringsAreEqual(StubAssembler& jit)
{
    const GPRReg first = jit.operand(0);
    const GPRReg second = jit.operand(1);
    const GPRReg result = jit.result();
    const GPRReg scratch = jit.allocateScratch(StubRegister::T9);
    const GPRReg firstFiber = jit.allocateScratch(StubRegister::T13);
    const GPRReg secondFiber = jit.allocateScratch(StubRegister::T14);
    const GPRReg word = jit.allocateScratch(StubRegister::A1);
    const GPRReg length = result;

    JumpList areEqual;
    JumpList areNotEqual;
    JumpList needsLeaf;
    areEqual.append(jit.branchPtr(CCallHelpers::Equal, first, second));
    jit.bindEntry(distinctStringsAreEqual());
    jit.loadPtr(Address(first, JSString::offsetOfValue()), firstFiber);
    jit.loadPtr(Address(second, JSString::offsetOfValue()), secondFiber);
    jit.or64(firstFiber, secondFiber, scratch);
    Jump oneIsRope = jit.branchTestPtr(CCallHelpers::NonZero, scratch, TrustedImm32(JSString::isRopeInPointer));
    areEqual.append(jit.branchPtr(CCallHelpers::Equal, firstFiber, secondFiber));
    jit.load32(Address(firstFiber, StringImpl::lengthMemoryOffset()), length);
    areNotEqual.append(jit.branch32(CCallHelpers::NotEqual, Address(secondFiber, StringImpl::lengthMemoryOffset()), length));
    jit.load32(Address(firstFiber, StringImpl::flagsOffset()), scratch);
    jit.and32(Address(secondFiber, StringImpl::flagsOffset()), scratch);
    areNotEqual.append(jit.branchTest32(CCallHelpers::NonZero, scratch, TrustedImm32(StringImpl::flagIsAtom())));
    branchIfCharactersDiffer(jit, { firstFiber, secondFiber, length, scratch, word }, areNotEqual, needsLeaf);

    areEqual.link(&jit);
    returnResult(jit, TrustedImm32(1));

    areNotEqual.link(&jit);
    Label returnFalse = jit.label();
    returnResult(jit, TrustedImm32(0));

    needsLeaf.link(&jit);
    compareImplsAndReturn(jit, Entry::operationAOTStringImplsAreEqual, firstFiber, secondFiber);

    oneIsRope.link(&jit);
    loadStringLength(jit, first, firstFiber, length);
    loadStringLength(jit, second, secondFiber, scratch);
    jit.branch32(CCallHelpers::NotEqual, length, scratch).linkTo(returnFalse, &jit);
    JumpList differ;
    JumpList needsOperation;
    branchIfCharactersInPlaceDiffer(jit, { first, second, firstFiber, secondFiber, length, scratch, word }, differ, needsOperation);
    returnResult(jit, TrustedImm32(1));

    differ.linkTo(returnFalse, &jit);
    needsOperation.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTStringsAreEqual, { .saves = Saves::CallerSavedRegisters, .checksException = false, .secondArgumentIn = first, .thirdArgumentIn = second });
}

void branchOnOrderOfFlatStrings(StubAssembler& jit, const OrderedFlatStrings& strings, JumpList& needsLeaf, JumpList& isLess, JumpList& isEqual)
{
    auto [first, second, firstImpl, secondImpl, length, firstWord, secondWord] = strings;
    RELEASE_ASSERT(noOverlap(first, second, firstImpl, secondImpl, length, firstWord, secondWord));

    JumpList differ;
    jit.load32(Address(firstImpl, StringImpl::flagsOffset()), firstWord);
    jit.and32(Address(secondImpl, StringImpl::flagsOffset()), firstWord);
    needsLeaf.append(jit.branchTest32(CCallHelpers::Zero, firstWord, TrustedImm32(StringImpl::flagIs8Bit())));
    jit.load32(Address(firstImpl, StringImpl::lengthMemoryOffset()), length);
    jit.load32(Address(secondImpl, StringImpl::lengthMemoryOffset()), firstWord);
    jit.moveConditionally32(CCallHelpers::Above, length, firstWord, firstWord, length);
    needsLeaf.append(jit.branch32(CCallHelpers::Above, length, TrustedImm32(maxLengthComparedWithoutCall)));
    jit.loadPtr(Address(firstImpl, StringImpl::dataOffset()), firstImpl);
    jit.loadPtr(Address(secondImpl, StringImpl::dataOffset()), secondImpl);
    branchIfBytesDiffer(jit, { firstImpl, secondImpl, length, firstWord, secondWord }, differ);

    jit.loadPtr(Address(first, JSString::offsetOfValue()), firstImpl);
    jit.loadPtr(Address(second, JSString::offsetOfValue()), secondImpl);
    jit.load32(Address(firstImpl, StringImpl::lengthMemoryOffset()), firstWord);
    jit.load32(Address(secondImpl, StringImpl::lengthMemoryOffset()), secondWord);
    isEqual.append(jit.branch32(CCallHelpers::Equal, firstWord, secondWord));
    Jump compareWords = jit.jump();

    differ.link(&jit);
    jit.byteSwap64(firstWord);
    jit.byteSwap64(secondWord);
    compareWords.link(&jit);
    isLess.append(jit.branch64(CCallHelpers::Below, firstWord, secondWord));
}

// The result is -1, 0 or 1, sign-extended to 64 bits.
void generateCompareStrings(StubAssembler& jit)
{
    const GPRReg first = jit.operand(0);
    const GPRReg second = jit.operand(1);
    const GPRReg result = jit.result();
    const GPRReg scratch = jit.allocateScratch(StubRegister::T9);
    const GPRReg firstFiber = jit.allocateScratch(StubRegister::T13);
    const GPRReg secondFiber = jit.allocateScratch(StubRegister::T14);
    const GPRReg word = jit.allocateScratch(StubRegister::A1);

    JumpList needsLeaf;
    JumpList isLess;
    JumpList isEqual;
    isEqual.append(jit.branchPtr(CCallHelpers::Equal, first, second));
    jit.loadPtr(Address(first, JSString::offsetOfValue()), firstFiber);
    jit.loadPtr(Address(second, JSString::offsetOfValue()), secondFiber);
    jit.or64(firstFiber, secondFiber, scratch);
    Jump oneIsRope = jit.branchTestPtr(CCallHelpers::NonZero, scratch, TrustedImm32(JSString::isRopeInPointer));
    branchOnOrderOfFlatStrings(jit, { first, second, firstFiber, secondFiber, result, scratch, word }, needsLeaf, isLess, isEqual);
    returnResult(jit, TrustedImm32(1));

    isLess.link(&jit);
    returnResult(jit, TrustedImm64(-1));

    isEqual.link(&jit);
    returnResult(jit, TrustedImm32(0));

    needsLeaf.link(&jit);
    compareImplsAndReturn(jit, Entry::operationAOTCompareStringImpls, firstFiber, secondFiber);

    oneIsRope.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTCompareStrings, { .saves = Saves::CallerSavedRegisters, .secondArgumentIn = first, .thirdArgumentIn = second });
}

// The index of the first unit from position on that equals codeUnit, or -1. Only bits 0 to 31 of length, codeUnit and position count.
void generateFindCharacter8(StubAssembler& jit)
{
    static_assert(leafArgumentGPR(0) == gprOf(StubRegister::A0) && leafArgumentGPR(1) == gprOf(StubRegister::A1));
    static_assert(leafArgumentGPR(2) == gprOf(StubRegister::A2) && leafArgumentGPR(3) == gprOf(StubRegister::A3));
    const GPRReg characters = jit.operand(0);
    const GPRReg length = jit.operand(1);
    const GPRReg codeUnit = jit.operand(2);
    const GPRReg position = jit.operand(3);
    const GPRReg result = jit.result();
    // On ARM64 the result shares its register with characters, and the leaf clobbers A4 and A5 anyway.
    const GPRReg index = result == characters ? jit.allocateScratch(StubRegister::A5) : result;
    const GPRReg unit = jit.allocateScratch(StubRegister::A4);
    RELEASE_ASSERT(result == GPRInfo::returnValueGPR);
    RELEASE_ASSERT(noOverlap(characters, length, codeUnit, position, index, unit));

    Jump isPastEnd = jit.branch32(CCallHelpers::AboveOrEqual, position, length);
    Jump needsLeaf = jit.branch32(CCallHelpers::Above, length, TrustedImm32(maxLengthSearchedByBytes));
    jit.zeroExtend32ToWord(position, index);
    Label next = jit.label();
    jit.load8(BaseIndex(characters, index, CCallHelpers::TimesOne), unit);
    Jump isFound = jit.branch32(CCallHelpers::Equal, unit, codeUnit);
    jit.add32(TrustedImm32(1), index);
    jit.branch32(CCallHelpers::Below, index, length).linkTo(next, &jit);

    isPastEnd.link(&jit);
    returnResult(jit, TrustedImm64(-1));

    isFound.link(&jit);
    returnResult(jit, index);

    needsLeaf.link(&jit);
    jit.callLeaf(Entry::operationAOTStringFindCharacter8);
    jit.returnToCaller();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
