/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubHelpers.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTInstance.h"
#include "AOTRuntimeWords.h"
#include "AOTSlotRepresentation.h"
#include "AOTStubRegisters.h"
#include "IndexingType.h"
#include "JSCell.h"
#include "JSType.h"
#include "JSTypeInfo.h"
#include "MarkedBlock.h"
#include "PreciseAllocation.h"
#include "Structure.h"
#include "Watchpoint.h"
#include "WeakSet.h"

namespace JSC { namespace AOT {

void unboxDouble(StubAssembler& jit, GPRReg boxed, GPRReg scratch, FPRReg result)
{
    jit.unboxDoubleWithoutAssertions(boxed, scratch, result);
}

PureDouble PureDouble::branchIfNaN(StubAssembler& jit, FPRReg number, JumpList& isNaN)
{
    isNaN.append(jit.branchIfNaN(number));
    return PureDouble(number);
}

PureDouble PureDouble::branchIfNaN(StubAssembler& jit, FPRReg number, Jump& isNaN)
{
    isNaN = jit.branchIfNaN(number);
    return PureDouble(number);
}

PureDouble PureDouble::purify(StubAssembler& jit, FPRReg number, FPRReg result)
{
    jit.purifyNaN(number, result);
    return PureDouble(result);
}

PureDouble PureDouble::convertInt64(StubAssembler& jit, GPRReg integer, FPRReg result)
{
    jit.convertInt64ToDouble(integer, result);
    return PureDouble(result);
}

PureDoubleBits PureDoubleBits::branchIfNotFinite(StubAssembler& jit, GPRReg bits, GPRReg scratch, JumpList& isNotFinite)
{
    jit.urshift64(bits, TrustedImm32(52), scratch);
    jit.and32(TrustedImm32(0x7ff), scratch);
    isNotFinite.append(jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(0x7ff)));
    return PureDoubleBits(bits);
}

static void subtractNumberTag(StubAssembler& jit, GPRReg bitsAndResult)
{
    jit.withNumberTag([&](auto tag) {
        jit.sub64(tag, bitsAndResult);
    });
}

void boxDouble(StubAssembler& jit, PureDouble number, GPRReg result)
{
    jit.moveDoubleTo64(number.fpr(), result);
    subtractNumberTag(jit, result);
}

void boxDoubleBits(StubAssembler& jit, PureDoubleBits bitsAndResult)
{
    subtractNumberTag(jit, bitsAndResult.gpr());
}

void boxInt32(StubAssembler& jit, GPRReg int32, GPRReg result)
{
#if CPU(ARM64)
    jit.addZeroExtend64(GPRInfo::numberTagRegister, int32, result);
#else
    jit.zeroExtend32ToWord(int32, result);
    jit.or64(numberTag, result);
#endif
}

void toDouble(StubAssembler& jit, GPRReg boxedNumber, GPRReg scratch, FPRReg result)
{
    Jump isInt32 = jit.branch64(CCallHelpers::AboveOrEqual, boxedNumber, numberTag);
    unboxDouble(jit, boxedNumber, scratch, result);
    Jump done = jit.jump();
    isInt32.link(&jit);
    jit.convertInt32ToDouble(boxedNumber, result);
    done.link(&jit);
}

void branchIfNotExactInt32(StubAssembler& jit, FPRReg number, GPRReg integer, JumpList& isNotExact, FPRReg scratch)
{
    ASSERT(noOverlap(number, scratch));
#if CPU(ARM64)
    if (jit.supportsDoubleToInt32ConversionUsingJavaScriptSemantics()) {
        jit.convertDoubleToInt32UsingJavaScriptSemantics(number, integer);
        isNotExact.append(jit.branchOnFlags(CCallHelpers::NotEqual));
        return;
    }
#endif
#if CPU(X86_64)
    // The integer, made a double again, has the bits of the number, or the number is no int32: the bits of negative zero and of NaN differ from those of any integer.
    const GPRReg difference = jit.scratchRegister();
    RELEASE_ASSERT(integer != difference);
    jit.truncateDoubleToInt32(number, integer);
    jit.convertInt32ToDouble(integer, scratch);
    jit.xorDouble(number, scratch);
    jit.moveDoubleTo64(scratch, difference);
    isNotExact.append(jit.branchTest64(CCallHelpers::NonZero, difference));
#else
    jit.branchConvertDoubleToInt32(number, integer, isNotExact, scratch);
#endif
}

void boxExactInt32(StubAssembler& jit, GPRReg integer, GPRReg result)
{
#if CPU(X86_64)
    if (integer == result) {
        jit.or64(numberTag, result);
        return;
    }
#endif
    boxInt32(jit, integer, result);
}

void decodeCell32(StubAssembler& jit, GPRReg word, GPRReg result)
{
#if CPU(ARM64)
    jit.addLeftShift64(cellCageBaseGPR, word, TrustedImm32(cell32Shift), result);
#else
    jit.getEffectiveAddress(BaseIndex(cellCageBaseGPR, word, static_cast<CCallHelpers::Scale>(cell32Shift)), result);
#endif
}

void encodeCell32(StubAssembler& jit, GPRReg cell, GPRReg word)
{
    jit.urshift64(cell, TrustedImm32(cell32Shift), word);
}

void appendGranule(StubAssembler& jit, GPRReg extension, GPRReg granuleBits, GPRReg scratch, GPRReg secondScratch, JumpList& cannotAppend)
{
    RELEASE_ASSERT(noOverlap(extension, granuleBits, scratch, secondScratch));
    const Address heldBits(extension, offsetOfExtensionGranuleBits);
    jit.move(granuleBits, scratch);
    jit.sub64(TrustedImm32(1), scratch);
    cannotAppend.append(jit.branchTest64(CCallHelpers::NonZero, scratch, granuleBits));
    jit.load64(heldBits, scratch);
    cannotAppend.append(jit.branch64(CCallHelpers::BelowOrEqual, granuleBits, scratch));
    jit.countPopulation64(scratch, secondScratch, CCallHelpers::fpTempRegister);
    jit.move(TrustedImm64(static_cast<int64_t>(granuleCountsWithoutRoom)), scratch);
    jit.urshift64(secondScratch, scratch);
    cannotAppend.append(jit.branchTest64(CCallHelpers::NonZero, scratch, TrustedImm32(1)));
    jit.load64(heldBits, scratch);
    jit.or64(granuleBits, scratch);
    jit.store64(scratch, heldBits);
}

void loadLiteralString(StubAssembler& jit, GPRReg wordAndResult, GPRReg scratch, JumpList& isNotCreated)
{
    RELEASE_ASSERT(noOverlap(wordAndResult, scratch));
    if (jit.options().useWideReferences) {
        static_assert(2 << literalStringShift == sizeof(JSCell*));
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(runtime.constants.wideLiteralStrings)), scratch);
        jit.load64(BaseIndex(scratch, wordAndResult, CCallHelpers::TimesTwo, -2 * static_cast<int32_t>(emptyLiteralStringWord)), wordAndResult);
        isNotCreated.append(jit.branchTest64(CCallHelpers::Zero, wordAndResult));
        return;
    }
    static_assert(1 << literalStringShift == sizeof(uint32_t));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(runtime.constants.literalStrings)), scratch);
    jit.load32(BaseIndex(scratch, wordAndResult, CCallHelpers::TimesOne, -static_cast<int32_t>(emptyLiteralStringWord)), wordAndResult);
    isNotCreated.append(jit.branchTest32(CCallHelpers::Zero, wordAndResult));
    decodeCell32(jit, wordAndResult, wordAndResult);
}

void decodeStructureID(CCallHelpers& jit, GPRReg idAndResult)
{
    jit.add64(Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(structureIDBase)), idAndResult);
}

void loadStructure(StubAssembler& jit, GPRReg cell, GPRReg result)
{
    jit.load32(Address(cell, JSCell::structureIDOffset()), result);
    decodeStructureID(jit, result);
}

void extractBits(StubAssembler& jit, GPRReg source, unsigned shift, unsigned width, GPRReg result)
{
#if CPU(ARM64)
    jit.extractUnsignedBitfield64(source, TrustedImm32(shift), TrustedImm32(width), result);
#else
    RELEASE_ASSERT(width < 32);
    jit.move(source, result);
    jit.urshift64(TrustedImm32(shift), result);
    jit.and64(TrustedImm32((1 << width) - 1), result);
#endif
}

void loadShapeWord(StubAssembler& jit, GPRReg cell, GPRReg result)
{
    jit.load32(Address(cell, offsetOfShapeWord), result);
}

void loadCellVM(StubAssembler& jit, GPRReg cellAndResult)
{
    Jump isPreciseAllocation = jit.branchTestPtr(CCallHelpers::NonZero, cellAndResult, TrustedImm32(PreciseAllocation::halfAlignment));
    jit.and64(TrustedImm64(static_cast<int64_t>(MarkedBlock::blockMask)), cellAndResult);
    jit.loadPtr(Address(cellAndResult, MarkedBlock::offsetOfHeader + MarkedBlock::Header::offsetOfVM()), cellAndResult);
    Label done = jit.label();
    jit.addOutOfLinePart([=](StubAssembler& jit) {
        isPreciseAllocation.link(&jit);
        jit.loadPtr(Address(cellAndResult, PreciseAllocation::offsetOfWeakSet() + WeakSet::offsetOfVM() - PreciseAllocation::headerSize()), cellAndResult);
        jit.jump().linkTo(done, &jit);
    });
}

Jump branchIfNotOfThisRealm(StubAssembler& jit, GPRReg structure)
{
    jit.loadPtr(Address(structure, Structure::realmOffset()), structure);
    return jit.branchPtr(CCallHelpers::NotEqual, structure, Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject)));
}

Jump branchIfNotPristine(StubAssembler& jit, GPRReg cell)
{
    return jit.branchTest8(CCallHelpers::Zero, Address(cell, JSCell::typeInfoFlagsOffset()), TrustedImm32(IsPristine));
}

void loadPristineArrayKind(StubAssembler& jit, GPRReg array, GPRReg kind, unsigned mask)
{
    static_assert(JSCell::typeInfoFlagsOffset() == JSCell::indexingTypeAndMiscOffset() + 2);
    constexpr uint32_t pristineBit = IsPristine << 16;
    RELEASE_ASSERT(mask <= 0xff);
    uint32_t bits = mask == IndexingShapeAndWritabilityMask ? 0xff & ~IsArray : mask;
    jit.load32(Address(array, JSCell::indexingTypeAndMiscOffset()), kind);
    jit.and32(TrustedImm32(bits | pristineBit), kind);
    jit.xor32(TrustedImm32(pristineBit), kind);
}

Jump branchIfWatchpointSetIsNotWatched(StubAssembler& jit, Address set, GPRReg scratch)
{
    constexpr unsigned stateShift = 1;
    static_assert(InlineWatchpointSet::encodeState(ClearWatchpoint) == (static_cast<uintptr_t>(ClearWatchpoint) << stateShift | InlineWatchpointSet::IsThinFlag));
    static_assert(InlineWatchpointSet::encodeState(IsWatched) == (static_cast<uintptr_t>(IsWatched) << stateShift | InlineWatchpointSet::IsThinFlag));
    static_assert(InlineWatchpointSet::encodeState(IsInvalidated) == (static_cast<uintptr_t>(IsInvalidated) << stateShift | InlineWatchpointSet::IsThinFlag));
    jit.loadPtr(set.withOffset(InlineWatchpointSet::offsetOfData()), scratch);
    Jump isThin = jit.branchTestPtr(CCallHelpers::NonZero, scratch, TrustedImm32(InlineWatchpointSet::IsThinFlag));
    jit.load8(Address(scratch, WatchpointSet::offsetOfState()), scratch);
    jit.lshift32(TrustedImm32(stateShift), scratch);
    jit.or32(TrustedImm32(InlineWatchpointSet::IsThinFlag), scratch);
    isThin.link(&jit);
    return jit.branch64(CCallHelpers::NotEqual, scratch, TrustedImm32(static_cast<int32_t>(InlineWatchpointSet::encodeState(IsWatched))));
}

Jump branchIfNotFunctionType(CCallHelpers& jit, GPRReg cell, GPRReg scratch)
{
    static_assert(slimClosureTypes.first > functionTypes.last);
    RELEASE_ASSERT(cell != scratch);
    jit.load8(Address(cell, JSCell::typeInfoTypeOffset()), scratch);
    jit.sub32(TrustedImm32(functionTypes.first), scratch);
    Jump isFunction = jit.branch32(CCallHelpers::BelowOrEqual, scratch, TrustedImm32(functionTypes.last - functionTypes.first));
    jit.sub32(TrustedImm32(slimClosureTypes.first - functionTypes.first), scratch);
    Jump isNotFunction = jit.branch32(CCallHelpers::Above, scratch, TrustedImm32(slimClosureTypes.last - slimClosureTypes.first));
    isFunction.link(&jit);
    return isNotFunction;
}

void bumpRuntimeCounter(StubAssembler& jit, RuntimeCounterID counter, GPRReg scratch)
{
    RELEASE_ASSERT(counter != RuntimeCounterID::None);
    const Address word(instanceGPR, AOT_INSTANCE_OFFSET(runtime.counters) + CounterWords::offsetOfValue(counter));
#if CPU(X86_64)
    UNUSED_PARAM(scratch);
    jit.add64(TrustedImm32(1), word);
#else
    jit.load64(word, scratch);
    jit.add64(TrustedImm32(1), scratch);
    jit.store64(scratch, word);
#endif
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
