/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsStrings.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "JSString.h"
#include "RegisterSet.h"
#include "SmallStrings.h"
#include <wtf/Vector.h>
#include <wtf/text/StringImpl.h>

namespace JSC { namespace AOT {

static_assert(StringImpl::flagIs8Bit() == JSRopeString::is8BitInPointer);
static_assert(JSRopeString::offsetOfFiber0() == sizeof(EncodedJSValue) && JSRopeString::offsetOfLength() == 2 * sizeof(EncodedJSValue));
static_assert(maxSingleCharacterString == 0xff);

AssemblyHelpers::FreeListRefill makeSubstringOfFlatString(StubAssembler& jit, const FlatSubstringRegisters& registers, JumpList& fails)
{
    const auto [string, start, length, result, firstScratch, secondScratch, alsoKept] = registers;
    RELEASE_ASSERT(noOverlap(string, start, length, result, firstScratch, secondScratch, instanceGPR) && result == GPRInfo::returnValueGPR);
    RegisterSet lostToLeaf = registersClobberedByPreserveMostLeaves();
    lostToLeaf.add(leafArgumentGPR(0), IgnoreVectors);
    lostToLeaf.add(leafArgumentGPR(1), IgnoreVectors);
    RegisterSet kept = alsoKept;
    kept.merge(RegisterSet(string, start, length));
    kept.filter(lostToLeaf);
    Vector<GPRReg, 8> savedAroundLeaf;
    kept.forEach([&](Reg reg) {
        savedAroundLeaf.append(reg.gpr());
    });

    jit.loadPtr(Address(string, JSString::offsetOfValue()), firstScratch);
    Jump isWhole = jit.branch32(CCallHelpers::Equal, length, Address(firstScratch, StringImpl::lengthMemoryOffset()));
    Jump isAtMostTwo = jit.branch32(CCallHelpers::BelowOrEqual, length, TrustedImm32(2));

    Label makesRope = jit.label();
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(ropeStringAllocator)), firstScratch);
    Label hasAllocator = jit.label();
    fails.append(jit.branchTestPtr(CCallHelpers::Zero, firstScratch));
    AssemblyHelpers::FreeListRefill refill = allocateCellWithLateRefill(jit, result, firstScratch, secondScratch, fails);
    jit.loadPtr(Address(string, JSString::offsetOfValue()), firstScratch);
    jit.load32(Address(firstScratch, StringImpl::flagsOffset()), firstScratch);
    jit.and32(TrustedImm32(JSRopeString::is8BitInPointer), firstScratch);
    jit.or64(TrustedImm32(JSString::isRopeInPointer | JSRopeString::isSubstringInPointer), firstScratch);
    jit.load64(Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(stringHeader)), secondScratch);
    jit.storePair64(secondScratch, firstScratch, result, TrustedImm32(0));
    jit.move(string, firstScratch);
    packSubstringFibers(jit, firstScratch, start, length, secondScratch);
    jit.storePair64(secondScratch, firstScratch, result, TrustedImm32(2 * sizeof(EncodedJSValue)));
    Label isMade = jit.label();

    jit.addOutOfLinePart([registers, savedAroundLeaf, isAtMostTwo, isWhole, makesRope, hasAllocator, isMade](StubAssembler& part) {
        const auto [string, start, length, result, characters, codeUnit, alsoKept] = registers;
        isWhole.link(&part);
        part.move(string, result);
        part.jump().linkTo(isMade, &part);

        isAtMostTwo.link(&part);
        Jump isAtMostOne = part.branch32(CCallHelpers::BelowOrEqual, length, TrustedImm32(1));
        // Two units of a 16-bit string make an 8-bit string if both are ASCII. The engine keeps the ones it has made, and the operation makes the others.
        part.branchTest32(CCallHelpers::NonZero, Address(characters, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())).linkTo(makesRope, &part);
        part.loadPtr(Address(characters, StringImpl::dataOffset()), characters);
        part.load32(BaseIndex(characters, start, CCallHelpers::TimesTwo), codeUnit);
        part.branchTest32(CCallHelpers::NonZero, codeUnit, TrustedImm32(static_cast<int32_t>(0xff80ff80))).linkTo(makesRope, &part);
        part.preserving(savedAroundLeaf.span(), [&] {
            part.recordClobbers(RegisterSet { leafArgumentGPR(0), leafArgumentGPR(1) });
            part.move(codeUnit, leafFunctionGPR);
            part.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), leafArgumentGPR(0));
            part.move(leafFunctionGPR, leafArgumentGPR(1));
            part.callLeaf(Entry::operationAOTFindKeptStringOfTwoCharacters, WTF::roundUpToMultipleOf<stackAlignmentBytes()>(savedAroundLeaf.size() * sizeof(CPURegister)));
        });
        part.branchTestPtr(CCallHelpers::NonZero, result).linkTo(isMade, &part);
        part.move(TrustedImm32(0), characters);
        part.jump().linkTo(hasAllocator, &part);

        isAtMostOne.link(&part);
        part.loadPtr(Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(emptyString)), result);
        part.branchTest32(CCallHelpers::Zero, length).linkTo(isMade, &part);
        part.load32(Address(characters, StringImpl::flagsOffset()), codeUnit);
        part.loadPtr(Address(characters, StringImpl::dataOffset()), characters);
        Jump isWide = part.branchTest32(CCallHelpers::Zero, codeUnit, TrustedImm32(StringImpl::flagIs8Bit()));
        part.load8(BaseIndex(characters, start, CCallHelpers::TimesOne), codeUnit);
        Jump hasCodeUnit = part.jump();
        isWide.link(&part);
        part.load16(BaseIndex(characters, start, CCallHelpers::TimesTwo), codeUnit);
        part.branch32(CCallHelpers::Above, codeUnit, TrustedImm32(maxSingleCharacterString)).linkTo(makesRope, &part);
        hasCodeUnit.link(&part);
        part.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(singleCharacterStrings)), characters);
        part.loadPtr(BaseIndex(characters, codeUnit, CCallHelpers::TimesEight), result);
        part.jump().linkTo(isMade, &part);
    });
    return refill;
}

StubSignature signatureOfSubstring(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2), gprOf(StubRegister::A3) }); }

void generateSubstring(StubAssembler& jit)
{
    const GPRReg string = jit.operand(0);
    const GPRReg from = jit.operand(1);
    const GPRReg to = jit.operand(2);
    const GPRReg result = jit.result();
    constexpr StubRegister spanRegister = StubRegister::T9;
#if CPU(ARM64)
    constexpr StubRegister fiberRegister = StubRegister::T11;
    const GPRReg flags = jit.allocateScratch(StubRegister::T12);
    const GPRReg base = jit.allocateScratch(StubRegister::T13);
    const GPRReg start = jit.allocateScratch(StubRegister::T14);
#else
    constexpr StubRegister fiberRegister = StubRegister::T10;
    const GPRReg flags = jit.allocateScratch(StubRegister::T11);
    const GPRReg base = jit.allocateScratch(StubRegister::T12);
    const GPRReg start = jit.allocateScratch(StubRegister::T13);
#endif
    const GPRReg span = jit.allocateScratch(spanRegister);
    const GPRReg fiber = jit.allocateScratch(fiberRegister);
    const GPRReg allocator = jit.reuseScratch(fiberRegister);
    const GPRReg scratch = jit.reuseScratch(spanRegister);

    JumpList fails;
    JumpList isWhole;
    jit.sub64(to, from, span);
    jit.loadPtr(Address(string, JSString::offsetOfValue()), fiber);
    Jump isRope = jit.branchIfRopeStringImpl(fiber);
    isWhole.append(jit.branch32(CCallHelpers::Equal, span, Address(fiber, StringImpl::lengthMemoryOffset())));
    Jump isAtMostTwo = jit.branch64(CCallHelpers::LessThanOrEqual, span, TrustedImm32(2));
    Label isLong = jit.label();
    jit.load32(Address(fiber, StringImpl::flagsOffset()), flags);
    jit.move(string, base);
    jit.move(from, start);
    Jump isReady = jit.jump();

    isRope.link(&jit);
    isWhole.append(jit.branch32(CCallHelpers::Equal, span, Address(string, JSRopeString::offsetOfLength())));
    Jump isAtMostTwoOfRope = jit.branch64(CCallHelpers::LessThanOrEqual, span, TrustedImm32(2));
    Label isLongPartOfRope = jit.label();
    fails.append(jit.branchTestPtr(CCallHelpers::Zero, fiber, TrustedImm32(JSRopeString::isSubstringInPointer)));
    jit.move(fiber, flags);
    loadSubstringBaseAndOffset(jit, string, base, start);
    jit.add64(from, start);

    isReady.link(&jit);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(ropeStringAllocator)), allocator);
    fails.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    AssemblyHelpers::FreeListRefill refill = allocateCellWithLateRefill(jit, result, allocator, scratch, fails);
    jit.load64(Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(stringHeader)), scratch);
    jit.and32(TrustedImm32(JSRopeString::is8BitInPointer), flags);
    jit.or64(TrustedImm32(JSString::isRopeInPointer | JSRopeString::isSubstringInPointer), flags);
    jit.storePair64(scratch, flags, result, TrustedImm32(0));
    jit.sub64(to, from, span);
    const GPRReg lengthWord = flags;
    const GPRReg upperWord = base;
    packSubstringFibers(jit, base, start, span, lengthWord);
    jit.storePair64(lengthWord, upperWord, result, TrustedImm32(2 * sizeof(EncodedJSValue)));
    fenceAfterAllocation(jit);
    jit.returnToCaller();

    isWhole.link(&jit);
    returnResult(jit, string);

    // Two units of a 16-bit string make an 8-bit string if both are ASCII. The operation looks at them.
    isAtMostTwoOfRope.link(&jit);
    Jump isNotTwoOfRope = jit.branch64(CCallHelpers::NotEqual, span, TrustedImm32(2));
    jit.branchTestPtr(CCallHelpers::NonZero, fiber, TrustedImm32(JSRopeString::is8BitInPointer)).linkTo(isLongPartOfRope, &jit);
    fails.append(jit.jump());
    isNotTwoOfRope.link(&jit);
    fails.append(jit.branch64(CCallHelpers::Equal, span, TrustedImm32(1)));
    Label isEmpty = jit.label();
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(emptyString)), result);
    jit.returnToCaller();

    isAtMostTwo.link(&jit);
    Jump isOne = jit.branch64(CCallHelpers::Equal, span, TrustedImm32(1));
    jit.branch64(CCallHelpers::LessThan, span, TrustedImm32(1)).linkTo(isEmpty, &jit);
    jit.branchTest32(CCallHelpers::NonZero, Address(fiber, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())).linkTo(isLong, &jit);
    fails.append(jit.jump());

    isOne.link(&jit);
    jit.load32(Address(fiber, StringImpl::flagsOffset()), flags);
    jit.loadPtr(Address(fiber, StringImpl::dataOffset()), fiber);
    Jump isWide = jit.branchTest32(CCallHelpers::Zero, flags, TrustedImm32(StringImpl::flagIs8Bit()));
    jit.load8(BaseIndex(fiber, from, CCallHelpers::TimesOne), flags);
    Jump hasCodeUnit = jit.jump();
    isWide.link(&jit);
    jit.load16(BaseIndex(fiber, from, CCallHelpers::TimesTwo), flags);
    fails.append(jit.branch32(CCallHelpers::Above, flags, TrustedImm32(maxSingleCharacterString)));
    hasCodeUnit.link(&jit);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(singleCharacterStrings)), fiber);
    jit.loadPtr(BaseIndex(fiber, flags, CCallHelpers::TimesEight), result);
    jit.returnToCaller();

    emitLateRefill(jit, refill, fails);
    fails.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTSubstring, { .returns = OperationCall::Returns::Value, .saves = OperationCall::Saves::CallerSavedRegisters });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
