/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsStrings.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTRuntimeTable.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
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

static void loadFlagsAndLength(StubAssembler& jit, GPRReg string, GPRReg flags, GPRReg length)
{
    jit.loadPtr(Address(string, JSString::offsetOfValue()), flags);
    Jump isRope = jit.branchIfRopeStringImpl(flags);
    jit.load32(Address(flags, StringImpl::lengthMemoryOffset()), length);
    jit.load32(Address(flags, StringImpl::flagsOffset()), flags);
    Jump isLoaded = jit.jump();
    isRope.link(&jit);
    jit.load32(Address(string, JSRopeString::offsetOfLength()), length);
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
    loadFlagsAndLength(jit, first, registers.flags, registers.length);
    Jump firstIsEmpty = jit.branchTest32(CCallHelpers::Zero, registers.length);
    loadFlagsAndLength(jit, second, registers.scratch, lengthOfSecond);
    Jump secondIsEmpty = jit.branchTest32(CCallHelpers::Zero, lengthOfSecond);
    jit.and32(registers.scratch, registers.flags);
    slow.append(jit.branchAdd32(CCallHelpers::Overflow, lengthOfSecond, registers.length));
    AssemblyHelpers::FreeListRefill refill = allocateAndFillRope(jit, registers, slow);
    jit.move(registers.rope, result);
    jit.returnToCaller();

    firstIsEmpty.link(&jit);
    jit.move(second, result);
    secondIsEmpty.link(&jit);
    jit.returnToCaller();

    emitLateRefill(jit, refill, slow);
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
    const GPRReg result = jit.result();
    const RopeRegisters registers {
        .first = jit.operand(0),
        .second = jit.operand(1),
        .third = jit.key().count() == 3 ? std::optional(jit.operand(2)) : std::nullopt,
        .flags = jit.allocateScratch(StubRegister::T9),
#if CPU(ARM64)
        .length = jit.allocateScratch(StubRegister::T14),
        .scratch = jit.allocateScratch(StubRegister::T15),
        .rope = result,
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
    Jump firstIsEmpty;
    Jump secondIsEmpty;
    Jump allAreEmpty;
    loadFlagsAndLength(jit, first, flags, length);
    if (!third)
        firstIsEmpty = jit.branchTest32(CCallHelpers::Zero, length);
    loadFlagsAndLength(jit, second, scratch, lengthOfNext);
    if (!third)
        secondIsEmpty = jit.branchTest32(CCallHelpers::Zero, lengthOfNext);
    jit.and32(scratch, flags);
    slow.append(jit.branchAdd32(CCallHelpers::Overflow, lengthOfNext, length));
    if (third) {
        loadFlagsAndLength(jit, *third, scratch, lengthOfNext);
        jit.and32(scratch, flags);
        slow.append(jit.branchAdd32(CCallHelpers::Overflow, lengthOfNext, length));
        allAreEmpty = jit.branchTest32(CCallHelpers::Zero, length);
    }
    AssemblyHelpers::FreeListRefill refill = allocateAndFillRope(jit, registers, slow);
    jit.move(rope, result);
    jit.returnToCaller();

    if (third) {
        allAreEmpty.link(&jit);
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(emptyString)), result);
        jit.returnToCaller();
    } else {
        firstIsEmpty.link(&jit);
        jit.move(second, result);
        jit.returnToCaller();
        secondIsEmpty.link(&jit);
        jit.move(first, result);
        jit.returnToCaller();
    }

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
