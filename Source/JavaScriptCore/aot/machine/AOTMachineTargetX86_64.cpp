/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineTarget.h"

#if ENABLE(AOT) && CPU(X86_64)

#include "AOTFaultTable.h"
#include "AOTInstance.h"
#include "AOTSavedFrame.h"
#include "AOTSlotRepresentation.h"
#include "AOTStubBodyConvention.h"
#include "JSCJSValue.h"
#include <algorithm>
#include <array>
#include <bit>
#include <limits>

namespace JSC { namespace AOT { namespace Machine {

template<typename... RegisterIDs>
static constexpr RegisterMask x86TargetMaskOf(RegisterIDs... registers)
{
    return (RegisterMask { 0 } | ... | maskOf(Reg(registers)));
}

template<typename RegisterID, size_t size>
static constexpr RegisterMask x86TargetMaskOfList(const std::array<RegisterID, size>& registers)
{
    RegisterMask mask = 0;
    for (RegisterID reg : registers)
        mask |= maskOf(Reg(reg));
    return mask;
}

static constexpr RegisterMask x86TargetRegistersIn(Bank bank)
{
    RegisterMask mask = 0;
    for (unsigned index = 0; index <= Reg::maxIndex(); ++index) {
        if (bankOf(Reg::fromIndex(index)) == bank)
            mask |= maskOf(Reg::fromIndex(index));
    }
    return mask;
}

static constexpr RegisterMask x86TargetPinnedRegisters = pinnedRegisters();
static constexpr RegisterMask x86TargetAllocatableGPRs = x86TargetMaskOf(
    X86Registers::eax, X86Registers::ecx, X86Registers::edx, X86Registers::ebx, X86Registers::esi, X86Registers::edi,
    X86Registers::r8, X86Registers::r9, X86Registers::r10, X86Registers::r12, X86Registers::r15);
static constexpr RegisterMask x86TargetAllocatableFPRs = x86TargetRegistersIn(Bank::FP) & ~x86TargetMaskOf(scratchFPR);
static constexpr RegisterMask x86TargetCalleeSavedGPRs = x86TargetMaskOf(X86Registers::ebx, X86Registers::r15);
static constexpr RegisterMask x86TargetGPRsClobberedAtSuspensions = x86TargetMaskOfList(suspensionClobberedGPRs);

static_assert(!(x86TargetAllocatableGPRs & ~x86TargetRegistersIn(Bank::GP)));
static_assert(x86TargetMaskOfList(allocatableGPRs) == x86TargetAllocatableGPRs && scratchImmediateGPR == scratchGPR);
static_assert((x86TargetRegistersIn(Bank::GP) & ~x86TargetAllocatableGPRs) == (x86TargetPinnedRegisters | x86TargetMaskOf(scratchGPR, MacroAssembler::stackPointerRegister, MacroAssembler::framePointerRegister)));
static_assert(!(x86TargetCalleeSavedGPRs & ~x86TargetAllocatableGPRs));
static_assert(!(x86TargetMaskOfList(unsavedCalleeSaveGPRs) & ~x86TargetAllocatableGPRs) && !(x86TargetMaskOfList(unsavedCalleeSaveGPRs) & x86TargetCalleeSavedGPRs));
static_assert(!(x86TargetGPRsClobberedAtSuspensions & ~x86TargetCalleeSavedGPRs));
static_assert((x86TargetCalleeSavedGPRs & ~x86TargetGPRsClobberedAtSuspensions) == x86TargetMaskOf(SavedFrame::preservedRegister));
static_assert(pinnedFPRsInFrameSavingFunctions.empty());
static_assert(registerWrittenAtFaults == scratchGPR);
static_assert(!static_cast<unsigned>(Bank::GP) && static_cast<unsigned>(Bank::FP) == 1);

struct X86TargetInstanceConstant {
    uint64_t bits;
    ptrdiff_t offset;
    bool replacesImmediates { false };
};

static constexpr std::array<X86TargetInstanceConstant, 18> x86TargetInstanceConstants { {
    { static_cast<uint64_t>(JSValue::NumberTag), AOT_INSTANCE_OFFSET(numberTag) },
    { static_cast<uint64_t>(JSValue::NotCellMask), AOT_INSTANCE_OFFSET(notCellMask) },
    { ~static_cast<uint64_t>(JSValue::ValueUndefined), AOT_INSTANCE_PREFIX_OFFSET(notUndefinedOrNullMask), true },
    { std::bit_cast<uint64_t>(1.0), AOT_INSTANCE_PREFIX_OFFSET(one) },
    { PNaNAsBits, AOT_INSTANCE_PREFIX_OFFSET(pureNaN) },
    { static_cast<uint64_t>(JSValue::DoubleEncodeOffset), AOT_INSTANCE_PREFIX_OFFSET(doubleEncodeOffset) },
    { static_cast<uint64_t>(JSValue::NumberTag | 1), AOT_INSTANCE_PREFIX_OFFSET(boxedOne) },
    { static_cast<uint64_t>(undefinedInteger), AOT_INSTANCE_PREFIX_OFFSET(undefinedInteger) },
    { static_cast<uint64_t>(nullInteger), AOT_INSTANCE_PREFIX_OFFSET(nullInteger) },
    { static_cast<uint64_t>(absentInteger), AOT_INSTANCE_PREFIX_OFFSET(absentInteger) },
    { undefinedDouble, AOT_INSTANCE_PREFIX_OFFSET(undefinedDouble) },
    { nullDouble, AOT_INSTANCE_PREFIX_OFFSET(nullDouble) },
    { absentDouble, AOT_INSTANCE_PREFIX_OFFSET(absentDouble) },
    { static_cast<uint64_t>(std::numeric_limits<int64_t>::min()), AOT_INSTANCE_PREFIX_OFFSET(minInt64) },
    { static_cast<uint64_t>(minIntegerAddedTo), AOT_INSTANCE_PREFIX_OFFSET(minIntegerAddedTo) },
    { static_cast<uint64_t>(maxIntegerAddedTo), AOT_INSTANCE_PREFIX_OFFSET(maxIntegerAddedTo) },
    { static_cast<uint64_t>(-minIntegerAddedTo), AOT_INSTANCE_PREFIX_OFFSET(distanceOfIntegersAddedToFromZero) },
    { static_cast<uint64_t>(maxIntegerAddedTo - minIntegerAddedTo), AOT_INSTANCE_PREFIX_OFFSET(spanOfIntegersAddedTo) },
} };

static constexpr uint64_t x86TargetSmallestInstanceConstant = [] {
    uint64_t smallest = std::numeric_limits<uint64_t>::max();
    for (const X86TargetInstanceConstant& constant : x86TargetInstanceConstants)
        smallest = std::min(smallest, constant.bits);
    return smallest;
}();

static constexpr std::array<uint64_t, x86TargetInstanceConstants.size()> x86TargetBitsOfInstanceConstants = [] {
    std::array<uint64_t, x86TargetInstanceConstants.size()> bits { };
    for (size_t index = 0; index < bits.size(); ++index)
        bits[index] = x86TargetInstanceConstants[index].bits;
    return bits;
}();

static_assert([] {
    for (const X86TargetInstanceConstant& constant : x86TargetInstanceConstants) {
        for (const X86TargetInstanceConstant& other : x86TargetInstanceConstants) {
            if (&constant != &other && (constant.bits == other.bits || constant.offset == other.offset))
                return false;
        }
    }
    return true;
}());

static_assert(AOT_INSTANCE_PREFIX_OFFSET(notUndefinedOrNullMask) >= std::numeric_limits<int8_t>::min() && AOT_INSTANCE_PREFIX_OFFSET(notUndefinedOrNullMask) <= std::numeric_limits<int8_t>::max());

static_assert(JSValue::ValueNull < JSValue::ValueUndefined && JSValue::ValueUndefined == undefinedCell32, "a trapping access faults for these two bases, and for what these two words decode to");
static_assert((undefinedCell32 << cell32Shift) < FaultSite::reach);
static constexpr uint32_t x86TargetTrapReach = static_cast<uint32_t>(FaultSite::reach - (undefinedCell32 << cell32Shift));

RegisterFile registerFileFor(const ProcedureTraits& traits)
{
    if (traits.savesRegistersOfCallers) {
        RegisterMask allocatableGPRs = x86TargetAllocatableGPRs & ~x86TargetCalleeSavedGPRs;
        return {
            .allocatable = { allocatableGPRs, x86TargetAllocatableFPRs },
            .calleeSaved = (allocatableGPRs | x86TargetAllocatableFPRs) & maskOf(StubBodyConvention::registersSavedByCallee()),
            .calleeSavedClobberedAtSuspensions = 0,
        };
    }
    return {
        .allocatable = { x86TargetAllocatableGPRs, x86TargetAllocatableFPRs },
        .calleeSaved = x86TargetCalleeSavedGPRs,
        .calleeSavedClobberedAtSuspensions = traits.savesFrameOnSuspend ? x86TargetGPRsClobberedAtSuspensions : 0,
    };
}

std::optional<PinnedConstant> findPinnedConstant(uint64_t bits)
{
    if (bits < x86TargetSmallestInstanceConstant)
        return std::nullopt;
    for (const X86TargetInstanceConstant& constant : x86TargetInstanceConstants) {
        if (constant.bits == bits)
            return PinnedConstant { .reg = instanceGPR, .offset = static_cast<int32_t>(constant.offset), .replacesImmediates = constant.replacesImmediates };
    }
    return std::nullopt;
}

std::span<const uint64_t> bitsOfPinnedConstants()
{
    return x86TargetBitsOfInstanceConstants;
}

bool isEncodableBitImmediate(Width width, int64_t bits)
{
    ASSERT(width <= Width64);
    if (fitsX86TargetImmediate(bits))
        return true;
    return width != Width64 && bits == static_cast<uint32_t>(bits);
}

bool acceptsMemoryDestination(const Operation& computation)
{
    switch (computation.opcode()) {
    case Add:
    case Sub:
    case BitAnd:
    case BitOr:
    case BitXor:
        return computation.type().isInt();

    case Nop:
    case Opaque:
    case Const32:
    case Const64:
    case ConstFloat:
    case ConstDouble:
    case FramePointer:
    case SlotBase:
    case AddressOf:
    case NumberOf:
    case InstanceAddressOf:
    case ArgumentReg:
    case Mul:
    case MulHigh:
    case UMulHigh:
    case Div:
    case UDiv:
    case Mod:
    case UMod:
    case Neg:
    case Shl:
    case SShr:
    case ZShr:
    case RotR:
    case Clz:
    case PopCount:
    case Abs:
    case Ceil:
    case Floor:
    case FTrunc:
    case Sqrt:
    case FMin:
    case FMax:
    case BitwiseCast:
    case SExt8:
    case SExt16:
    case SExt32:
    case ZExt32:
    case Trunc:
    case IToD:
    case FloatToDouble:
    case DoubleToFloat:
    case Equal:
    case NotEqual:
    case LessThan:
    case GreaterThan:
    case LessEqual:
    case GreaterEqual:
    case Above:
    case Below:
    case AboveEqual:
    case BelowEqual:
    case EqualOrUnordered:
    case Select:
    case Load8Z:
    case Load8S:
    case Load16Z:
    case Load16S:
    case Load:
    case Store8:
    case Store16:
    case Store:
    case Fence:
    case Phi:
    case Upsilon:
    case Extract:
    case Patch:
    case Check:
    case Jump:
    case Branch:
    case Switch:
    case Return:
    case Oops:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool isEncodableArithmeticConstant(int64_t constant)
{
    if (fitsX86TargetImmediate(constant))
        return true;
    std::optional<PinnedConstant> pinned = findPinnedConstant(static_cast<uint64_t>(constant));
    return pinned && pinned->offset;
}

bool isEncodableIndexedAddress(Width, unsigned shift, IndexExtension extension, int32_t)
{
    return shift <= 3 && extension == IndexExtension::None;
}

bool isEncodableScaledSum(unsigned shift, IndexExtension extension)
{
    return shift >= 1 && shift <= 3 && extension == IndexExtension::None;
}

bool scaledSumAcceptsBase(const Operation& base)
{
    return base.hasNoPosition() && base.opcode() == Add && base.child(0)->opcode() == ArgumentReg && fitsX86TargetImmediate(base.child(1)->asInt64());
}

unsigned numberOfScratchGPRsOf(const ProcedureTraits& traits, const Operation& operation)
{
    if (operation.opcode() == Switch)
        return operation.switchData().caseValues.size() >= minCasesForSwitchWindow ? 1 : 0;
    if (operation.opcode() == PopCount)
        return MacroAssembler::supportsCountPopulation(traits.features) ? 0 : 1;
    return 0;
}

uint32_t trapReach()
{
    return x86TargetTrapReach;
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT) && CPU(X86_64)
