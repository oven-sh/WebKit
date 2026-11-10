/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsStrings.h"

#if ENABLE(AOT)

#include "AOTGrowingText.h"
#include "AOTInstance.h"
#include "AOTRuntimeTable.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "AOTStubsConstants.h"
#include "AOTVector.h"
#include "JSString.h"
#include <limits>
#include <optional>
#include <wtf/text/StringImpl.h>

namespace JSC { namespace AOT {

static_assert(StringImpl::flagIs8Bit() == JSRopeString::is8BitInPointer);
static_assert(JSString::MaxLength == std::numeric_limits<int32_t>::max());
static_assert(JSRopeString::offsetOfFiber0() == sizeof(EncodedJSValue) && JSRopeString::offsetOfLength() == 2 * sizeof(EncodedJSValue));

struct RopeRegisters {
    GPRReg first;
    GPRReg second;
    std::optional<GPRReg> third;
    GPRReg flags;
    GPRReg length;
    GPRReg scratch;
    GPRReg rope;
    GPRReg allocator;
};

struct LateLengthOfRope {
    Jump isRope;
    Label isLoaded;
    GPRReg string;
    GPRReg flags;
    GPRReg length;
};

[[nodiscard]] static LateLengthOfRope loadFlagsAndLength(StubAssembler& jit, GPRReg string, GPRReg flags, GPRReg length, JumpList& isEmpty)
{
    jit.loadPtr(Address(string, JSString::offsetOfValue()), flags);
    Jump isRope = jit.branchIfRopeStringImpl(flags);
    jit.load32(Address(flags, StringImpl::lengthMemoryOffset()), length);
    jit.load32(Address(flags, StringImpl::flagsOffset()), flags);
    isEmpty.append(jit.branchTest32(CCallHelpers::Zero, length));
    return { isRope, jit.label(), string, flags, length };
}

static void emitLateLengthOfRope(StubAssembler& jit, const LateLengthOfRope& late, JumpList* mayGrow = nullptr)
{
    late.isRope.link(&jit);
    jit.load32(Address(late.string, JSRopeString::offsetOfLength()), late.length);
    if (mayGrow) {
        jit.branchTestPtr(CCallHelpers::Zero, late.flags, TrustedImm32(JSRopeString::isSubstringInPointer)).linkTo(late.isLoaded, &jit);
        mayGrow->append(jit.branch32(CCallHelpers::AboveOrEqual, late.length, TrustedImm32(minLengthOfGrowingText)));
    }
    jit.jump().linkTo(late.isLoaded, &jit);
}

[[nodiscard]] static AssemblyHelpers::FreeListRefill allocateRope(StubAssembler& jit, GPRReg rope, GPRReg allocator, GPRReg scratch, JumpList& slow)
{
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(ropeStringAllocator)), allocator);
    return allocateCellWithLateRefill(jit, rope, allocator, scratch, slow, sizeof(JSRopeString));
}

[[nodiscard]] static AssemblyHelpers::FreeListRefill allocateAndFillRope(StubAssembler& jit, const RopeRegisters& registers, JumpList& slow)
{
    const auto [first, second, third, flags, length, scratch, rope, allocator] = registers;
    ASSERT(noOverlap(first, second, third.value_or(InvalidGPRReg), flags, length, scratch, rope, allocator));
    AssemblyHelpers::FreeListRefill refill = allocateRope(jit, rope, allocator, scratch, slow);
    jit.load64(Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(stringHeader)), scratch);
    jit.and32(TrustedImm32(JSRopeString::is8BitInPointer), flags);
    jit.getEffectiveAddress(BaseIndex(first, flags, CCallHelpers::TimesOne, JSString::isRopeInPointer), flags);
    jit.storePair64(scratch, flags, rope, TrustedImm32(0));

    constexpr ptrdiff_t offsetOfUpperThird = JSRopeString::offsetOfFiber2Lower() + sizeof(uint16_t);
    static_assert(offsetOfUpperThird + sizeof(uint32_t) == sizeof(JSRopeString));
    jit.store32(length, Address(rope, JSRopeString::offsetOfLength()));
    jit.store64(second, Address(rope, JSRopeString::offsetOfFiber1Lower()));
    if (third) {
        jit.store16(*third, Address(rope, JSRopeString::offsetOfFiber2Lower()));
        jit.move(*third, scratch);
        jit.urshift64(TrustedImm32(16), scratch);
        jit.store32(scratch, Address(rope, offsetOfUpperThird));
    } else
        jit.store32(TrustedImm32(0), Address(rope, offsetOfUpperThird));
    fenceAfterAllocation(jit);
    return refill;
}

[[nodiscard]] static AssemblyHelpers::FreeListRefill appendInBufferAndReturn(StubAssembler& jit, const RopeRegisters& registers, JumpList& other)
{
    const auto [text, second, third, flags, length, scratch, rope, allocator] = registers;
    ASSERT(noOverlap(text, second, third.value_or(InvalidGPRReg), flags, length, scratch, rope, allocator));
    static_assert(JSRopeString::offsetOfFiber2() == JSRopeString::offsetOfLength() + static_cast<ptrdiff_t>(sizeof(uint64_t)));
    Vector<GPRReg, 2> pieces { second };
    if (third)
        pieces.append(*third);

    other.append(jit.branchTestPtr(CCallHelpers::Zero, flags, TrustedImm32(JSRopeString::is8BitInPointer)));
    const GPRReg characters = flags;
    const GPRReg room = scratch;
    {
        const GPRReg buffer = scratch;
        const GPRReg end = allocator;
        loadSubstringBaseAndOffset(jit, text, buffer, end);
        jit.load32(Address(buffer, JSCell::structureIDOffset()), rope);
        other.append(jit.branch32(CCallHelpers::NotEqual, rope, Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.textBufferStructureID))));
        jit.loadPtr(Address(buffer, JSString::offsetOfValue()), buffer);
        other.append(jit.branchIfRopeStringImpl(buffer));
        jit.add32(length, end);
        jit.loadPtr(Address(buffer, StringImpl::dataOffset()), characters);
        other.append(jit.branch32(CCallHelpers::NotEqual, end, Address(characters, offsetOfEndOfTextInCharactersOfBuffer)));
        jit.load32(Address(buffer, StringImpl::lengthMemoryOffset()), room);
        jit.sub32(end, room);
    }
    const GPRReg lengthOfPieces = length;
    for (size_t index = 0; index < pieces.size(); ++index) {
        const GPRReg impl = rope;
        const GPRReg lengthOfPiece = index ? allocator : lengthOfPieces;
        jit.loadPtr(Address(pieces[index], JSString::offsetOfValue()), impl);
        other.append(jit.branchIfRopeStringImpl(impl));
        other.append(jit.branchTest32(CCallHelpers::Zero, Address(impl, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
        jit.load32(Address(impl, StringImpl::lengthMemoryOffset()), lengthOfPiece);
        other.append(jit.branch32(CCallHelpers::Above, lengthOfPiece, TrustedImm32(maxLengthOfPieceWrittenByStub)));
        if (index)
            jit.add32(lengthOfPiece, lengthOfPieces);
    }
    other.append(jit.branch32(CCallHelpers::Above, lengthOfPieces, room));
    AssemblyHelpers::FreeListRefill refill = allocateRope(jit, rope, allocator, scratch, other);

    jit.load32(Address(characters, offsetOfEndOfTextInCharactersOfBuffer), scratch);
    jit.add32(lengthOfPieces, scratch);
    jit.store32(scratch, Address(characters, offsetOfEndOfTextInCharactersOfBuffer));
    const GPRReg destination = characters;
    jit.addPtr(scratch, destination);

    jit.load64(Address(text, JSRopeString::offsetOfLength()), scratch);
    jit.add64(lengthOfPieces, scratch);
    jit.load64(Address(text, JSRopeString::offsetOfFiber2()), allocator);
    jit.storePair64(scratch, allocator, rope, TrustedImm32(2 * sizeof(EncodedJSValue)));
    jit.load64(Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(stringHeader)), scratch);
    jit.move(TrustedImm32(JSString::isRopeInPointer | JSRopeString::isSubstringInPointer | JSRopeString::is8BitInPointer), allocator);
    jit.storePair64(scratch, allocator, rope, TrustedImm32(0));

    JumpList pieceHasBecomeRope;
    for (size_t index = pieces.size(); index--;) {
        const GPRReg source = length;
        const GPRReg count = allocator;
        jit.loadPtr(Address(pieces[index], JSString::offsetOfValue()), source);
        pieceHasBecomeRope.append(jit.branchIfRopeStringImpl(source));
        jit.load32(Address(source, StringImpl::lengthMemoryOffset()), count);
        jit.loadPtr(Address(source, StringImpl::dataOffset()), source);
        Jump isEmpty = jit.branchTest32(CCallHelpers::Zero, count);
        jit.subPtr(count, destination);
        Label next = jit.label();
        jit.sub32(TrustedImm32(1), count);
        jit.load8(BaseIndex(source, count, CCallHelpers::TimesOne), scratch);
        jit.store8(scratch, BaseIndex(destination, count, CCallHelpers::TimesOne));
        jit.branchTest32(CCallHelpers::NonZero, count).linkTo(next, &jit);
        isEmpty.link(&jit);
    }
    fenceAfterAllocation(jit);
    returnResult(jit, rope);

    pieceHasBecomeRope.link(&jit);
    jit.breakpoint();
    return refill;
}

static void jumpToRopeOfSubstring(StubAssembler& jit, const LateLengthOfRope& late)
{
    jit.load32(Address(late.string, JSRopeString::offsetOfLength()), late.length);
    jit.loadPtr(Address(late.string, JSString::offsetOfValue()), late.flags);
    jit.branchIfRopeStringImpl(late.flags).linkTo(late.isLoaded, &jit);
    jit.breakpoint();
}

void makeRopeOfTwoValuesAndReturn(StubAssembler& jit, GPRReg first, GPRReg second, JumpList& slow)
{
    const GPRReg result = jit.result();
    RELEASE_ASSERT(first == result);
    const RopeRegisters registers {
        .first = first,
        .second = second,
        .third = std::nullopt,
        .flags = jit.reuseScratch(StubRegister::T9),
        .length = jit.reuseScratch(StubRegister::T10),
        .scratch = jit.reuseScratch(StubRegister::T11),
        .rope = jit.reuseScratch(StubRegister::T12),
        .allocator = jit.reuseScratch(StubRegister::T13),
    };
    const GPRReg lengthOfSecond = registers.rope;

    slow.append(jit.branchIfNotCell(first));
    slow.append(jit.branchIfNotCell(second));
    slow.append(jit.branchIfNotString(first));
    slow.append(jit.branchIfNotString(second));
    JumpList firstIsEmpty;
    JumpList secondIsEmpty;
    LateLengthOfRope lateLengthOfFirst = loadFlagsAndLength(jit, first, registers.flags, registers.length, firstIsEmpty);
    LateLengthOfRope lateLengthOfSecond = loadFlagsAndLength(jit, second, registers.scratch, lengthOfSecond, secondIsEmpty);
    jit.and32(registers.scratch, registers.flags);
    slow.append(jit.branchAdd32(CCallHelpers::Overflow, lengthOfSecond, registers.length));
    AssemblyHelpers::FreeListRefill refill = allocateAndFillRope(jit, registers, slow);
    returnResult(jit, registers.rope);

    firstIsEmpty.link(&jit);
    jit.move(second, result);
    secondIsEmpty.link(&jit);
    jit.returnToCaller();

    JumpList mayGrow;
    emitLateLengthOfRope(jit, lateLengthOfFirst, &mayGrow);
    emitLateLengthOfRope(jit, lateLengthOfSecond);

    mayGrow.link(&jit);
    JumpList doesNotGrowHere;
    AssemblyHelpers::FreeListRefill refillForSubstring = appendInBufferAndReturn(jit, registers, doesNotGrowHere);
    doesNotGrowHere.link(&jit);
    jumpToRopeOfSubstring(jit, lateLengthOfFirst);

    emitLateRefill(jit, refill, slow);
    emitLateRefill(jit, refillForSubstring, slow);
}

StubSignature signatureOfConcatenate(StubKey key)
{
#if CPU(ARM64)
    StubSignature signature = StubSignature::withGPRResult({ gprOf(StubRegister::T11), gprOf(StubRegister::T12) });
#else
    StubSignature signature = StubSignature::withGPRResult({ gprOf(StubRegister::R0), gprOf(StubRegister::A2) });
#endif
    if (key.count() == 3)
        signature.addOperand(gprOf(StubRegister::A3));
    return signature;
}

void generateConcatenate(StubAssembler& jit)
{
    const RopeRegisters registers {
        .first = jit.operand(0),
        .second = jit.operand(1),
        .third = jit.key().count() == 3 ? std::optional(jit.operand(2)) : std::nullopt,
        .flags = jit.allocateScratch(StubRegister::T9),
#if CPU(ARM64)
        .length = jit.allocateScratch(StubRegister::T14),
        .scratch = jit.allocateScratch(StubRegister::T15),
        .rope = jit.result(),
#else
        .length = jit.allocateScratch(StubRegister::T10),
        .scratch = jit.allocateScratch(StubRegister::T11),
        .rope = jit.allocateScratch(StubRegister::T12),
#endif
        .allocator = jit.allocateScratch(StubRegister::T13),
    };
    const auto [first, second, third, flags, length, scratch, rope, allocator] = registers;
    const GPRReg lengthOfNext = rope;
    static_assert(gprOf(StubRegister::A3) == operationArgumentGPR(3));
    JumpList slow;
    Vector<AssemblyHelpers::FreeListRefill, 4> refills;
    Vector<LateLengthOfRope, 9> lateLengths;
    auto loadLengthOf = [&](GPRReg string) {
        JumpList isEmpty;
        lateLengths.append(loadFlagsAndLength(jit, string, flags, length, isEmpty));
        return isEmpty;
    };
    auto addLengthOf = [&](GPRReg string) {
        JumpList isEmpty;
        lateLengths.append(loadFlagsAndLength(jit, string, scratch, lengthOfNext, isEmpty));
        jit.and32(scratch, flags);
        slow.append(jit.branchAdd32(CCallHelpers::Overflow, lengthOfNext, length));
        return isEmpty;
    };
    auto joinTwoAndReturn = [&](GPRReg former, GPRReg latter) {
        RopeRegisters both = registers;
        both.first = former;
        both.second = latter;
        both.third = std::nullopt;
        JumpList formerIsEmpty = loadLengthOf(former);
        JumpList latterIsEmpty = addLengthOf(latter);
        refills.append(allocateAndFillRope(jit, both, slow));
        returnResult(jit, rope);
        formerIsEmpty.link(&jit);
        returnResult(jit, latter);
        latterIsEmpty.link(&jit);
        returnResult(jit, former);
    };
    if (third) {
        JumpList firstIsEmpty = loadLengthOf(first);
        JumpList secondIsEmpty = addLengthOf(second);
        JumpList thirdIsEmpty = addLengthOf(*third);
        refills.append(allocateAndFillRope(jit, registers, slow));
        returnResult(jit, rope);

        firstIsEmpty.link(&jit);
        joinTwoAndReturn(second, *third);
        secondIsEmpty.link(&jit);
        joinTwoAndReturn(first, *third);
        thirdIsEmpty.link(&jit);
        joinTwoAndReturn(first, second);
    } else
        joinTwoAndReturn(first, second);

    JumpList mayGrow;
    const LateLengthOfRope lateLengthOfFirst = lateLengths.first();
    emitLateLengthOfRope(jit, lateLengthOfFirst, &mayGrow);
    for (const LateLengthOfRope& late : lateLengths.span().subspan(1))
        emitLateLengthOfRope(jit, late);

    mayGrow.link(&jit);
    JumpList doesNotGrowHere;
    refills.append(appendInBufferAndReturn(jit, registers, doesNotGrowHere));
    doesNotGrowHere.link(&jit);
    jumpToRopeOfSubstring(jit, lateLengthOfFirst);

    for (AssemblyHelpers::FreeListRefill& refill : refills)
        emitLateRefill(jit, refill, slow);
    slow.link(&jit);
    jit.callOperationAndReturn(third ? Entry::operationAOTConcatThree : Entry::operationAOTConcat, {
        .saves = OperationCall::Saves::CallerSavedRegisters,
        .secondArgumentIn = first,
        .thirdArgumentIn = second,
    });
}

StubSignature signatureOfConcatenateWithConstants(StubKey key)
{
    return signatureOfConcatenate(concatenate(numberOfOperands(static_cast<ConstantsOfConcatenation>(key.form()))));
}

void generateConcatenateWithConstants(StubAssembler& jit)
{
    const auto constants = static_cast<ConstantsOfConcatenation>(jit.key().form());
    Vector<GPRReg, 2> numbers;
    for (unsigned operand = 0; operand < numberOfOperands(constants); ++operand) {
        if (isConstantOperand(constants, operand))
            numbers.append(jit.clobberOperand(operand));
    }
    loadStringConstantsAndJump(jit, numbers.span(), concatenate(numberOfOperands(constants)));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
