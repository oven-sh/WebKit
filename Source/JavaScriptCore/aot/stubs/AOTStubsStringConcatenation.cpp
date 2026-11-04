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

// `mayGrow`: of the string that the others are added to. What is added to a long substring may have room behind it in a buffer: AOTGrowingText.h.
static void loadFlagsAndLength(StubAssembler& jit, GPRReg string, GPRReg flags, GPRReg length, JumpList* mayGrow = nullptr)
{
    jit.loadPtr(Address(string, JSString::offsetOfValue()), flags);
    Jump isRope = jit.branchIfRopeStringImpl(flags);
    jit.load32(Address(flags, StringImpl::lengthMemoryOffset()), length);
    jit.load32(Address(flags, StringImpl::flagsOffset()), flags);
    Jump isLoaded = jit.jump();
    isRope.link(&jit);
    jit.load32(Address(string, JSRopeString::offsetOfLength()), length);
    if (mayGrow) {
        Jump isNoSubstring = jit.branchTestPtr(CCallHelpers::Zero, flags, TrustedImm32(JSRopeString::isSubstringInPointer));
        mayGrow->append(jit.branch32(CCallHelpers::AboveOrEqual, length, TrustedImm32(minLengthOfGrowingText)));
        isNoSubstring.link(&jit);
    }
    isLoaded.link(&jit);
}

[[nodiscard]] static AssemblyHelpers::FreeListRefill allocateAndFillRope(StubAssembler& jit, const RopeRegisters& registers, JumpList& slow)
{
    using CompactFibers = JSRopeString::CompactFibers;
    const auto [first, second, third, flags, length, scratch, rope, allocator] = registers;
    ASSERT(noOverlap(first, second, third.value_or(InvalidGPRReg), flags, length, scratch, rope, allocator));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(ropeStringAllocator)), allocator);
    slow.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    AssemblyHelpers::FreeListRefill refill = allocateCellWithLateRefill(jit, rope, allocator, scratch, slow);
    jit.load64(Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(stringHeader)), scratch);
    jit.and32(TrustedImm32(JSRopeString::is8BitInPointer), flags);
    jit.or64(first, flags);
    jit.or64(TrustedImm32(JSString::isRopeInPointer), flags);
    jit.storePair64(scratch, flags, rope, TrustedImm32(0));
    jit.move(second, scratch);
    jit.lshift64(TrustedImm32(CompactFibers::shiftOfFiber1LowerInLengthWord), scratch);
    jit.or64(length, scratch);
    jit.move(second, allocator);
    jit.urshift64(TrustedImm32(CompactFibers::widthOfFiber1LowerInLengthWord), allocator);
    if (third) {
        jit.move(*third, flags);
        jit.lshift64(TrustedImm32(CompactFibers::shiftOfFiber2InUpperWord), flags);
        jit.or64(flags, allocator);
    }
    jit.storePair64(scratch, allocator, rope, TrustedImm32(2 * sizeof(EncodedJSValue)));
    fenceAfterAllocation(jit);
    return refill;
}

// The first string is a substring of at least minLengthOfGrowingText, `flags` is its first word and `length` its length.
// Where it ends the text of an 8-bit buffer that has room for the other strings, and those are flat, 8-bit and short, they are written behind it, and the substring that holds them too is returned.
// Whatever else is the case leaves by `other`. Nothing was written then, and the strings lie where they lay: they become a rope like any others.
// The operation that a stub without cells calls writes all such ropes behind the text at once.
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
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(ropeStringAllocator)), allocator);
    other.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    AssemblyHelpers::FreeListRefill refill = allocateCellWithLateRefill(jit, rope, allocator, scratch, other);

    jit.load32(Address(characters, offsetOfEndOfTextInCharactersOfBuffer), scratch);
    jit.add32(lengthOfPieces, scratch);
    jit.store32(scratch, Address(characters, offsetOfEndOfTextInCharactersOfBuffer));
    const GPRReg destination = characters;
    jit.addPtr(scratch, destination);

    // The new substring has the buffer and the offset of the old one.
    jit.load64(Address(text, JSRopeString::offsetOfLength()), scratch);
    jit.add64(lengthOfPieces, scratch);
    jit.load64(Address(text, JSRopeString::offsetOfFiber2()), allocator);
    jit.storePair64(scratch, allocator, rope, TrustedImm32(2 * sizeof(EncodedJSValue)));
    jit.load64(Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(stringHeader)), scratch);
    jit.move(TrustedImm32(JSString::isRopeInPointer | JSRopeString::isSubstringInPointer | JSRopeString::is8BitInPointer), allocator);
    jit.storePair64(scratch, allocator, rope, TrustedImm32(0));

    // From the last piece to the first, and each from its last character to its first. Each piece was flat above, and a flat string stays flat.
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
    JumpList mayGrow;
    loadFlagsAndLength(jit, first, registers.flags, registers.length, &mayGrow);
    Label makeRope = jit.label();
    Jump firstIsEmpty = jit.branchTest32(CCallHelpers::Zero, registers.length);
    loadFlagsAndLength(jit, second, registers.scratch, lengthOfSecond);
    Jump secondIsEmpty = jit.branchTest32(CCallHelpers::Zero, lengthOfSecond);
    jit.and32(registers.scratch, registers.flags);
    slow.append(jit.branchAdd32(CCallHelpers::Overflow, lengthOfSecond, registers.length));
    AssemblyHelpers::FreeListRefill refill = allocateAndFillRope(jit, registers, slow);
    returnResult(jit, registers.rope);

    firstIsEmpty.link(&jit);
    jit.move(second, result);
    secondIsEmpty.link(&jit);
    jit.returnToCaller();

    mayGrow.link(&jit);
    JumpList doesNotGrowHere;
    AssemblyHelpers::FreeListRefill refillForSubstring = appendInBufferAndReturn(jit, registers, doesNotGrowHere);
    doesNotGrowHere.link(&jit);
    loadFlagsAndLength(jit, first, registers.flags, registers.length);
    jit.jump().linkTo(makeRope, &jit);

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
    // Returns the jump that an empty string takes.
    auto addLengthOf = [&](GPRReg string) {
        loadFlagsAndLength(jit, string, scratch, lengthOfNext);
        Jump isEmpty = jit.branchTest32(CCallHelpers::Zero, lengthOfNext);
        jit.and32(scratch, flags);
        slow.append(jit.branchAdd32(CCallHelpers::Overflow, lengthOfNext, length));
        return isEmpty;
    };
    JumpList mayGrow;
    Label makeRope;
    auto joinTwoAndReturn = [&](GPRReg former, GPRReg latter, bool formerMayGrow) {
        RopeRegisters both = registers;
        both.first = former;
        both.second = latter;
        both.third = std::nullopt;
        loadFlagsAndLength(jit, former, flags, length, formerMayGrow ? &mayGrow : nullptr);
        if (formerMayGrow)
            makeRope = jit.label();
        Jump formerIsEmpty = jit.branchTest32(CCallHelpers::Zero, length);
        Jump latterIsEmpty = addLengthOf(latter);
        refills.append(allocateAndFillRope(jit, both, slow));
        returnResult(jit, rope);
        formerIsEmpty.link(&jit);
        returnResult(jit, latter);
        latterIsEmpty.link(&jit);
        returnResult(jit, former);
    };
    if (third) {
        loadFlagsAndLength(jit, first, flags, length, &mayGrow);
        makeRope = jit.label();
        Jump firstIsEmpty = jit.branchTest32(CCallHelpers::Zero, length);
        Jump secondIsEmpty = addLengthOf(second);
        Jump thirdIsEmpty = addLengthOf(*third);
        refills.append(allocateAndFillRope(jit, registers, slow));
        returnResult(jit, rope);

        // An empty string is no part of a rope: whoever reads the text of `path + "" + ""` would copy the path into a buffer of its own.
        firstIsEmpty.link(&jit);
        joinTwoAndReturn(second, *third, false);
        secondIsEmpty.link(&jit);
        joinTwoAndReturn(first, *third, false);
        thirdIsEmpty.link(&jit);
        joinTwoAndReturn(first, second, false);
    } else
        joinTwoAndReturn(first, second, true);

    mayGrow.link(&jit);
    JumpList doesNotGrowHere;
    refills.append(appendInBufferAndReturn(jit, registers, doesNotGrowHere));
    doesNotGrowHere.link(&jit);
    loadFlagsAndLength(jit, first, flags, length);
    jit.jump().linkTo(makeRope, &jit);

    for (AssemblyHelpers::FreeListRefill& refill : refills)
        emitLateRefill(jit, refill, slow);
    slow.link(&jit);
    jit.callOperationAndReturn(third ? Entry::operationAOTConcatThree : Entry::operationAOTConcat, {
        .saves = OperationCall::Saves::CallerSavedRegisters,
        .secondArgumentIn = first,
        .thirdArgumentIn = second,
    });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
