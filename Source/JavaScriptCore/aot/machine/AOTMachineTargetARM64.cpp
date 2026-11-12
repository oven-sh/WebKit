/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineTarget.h"

#if ENABLE(AOT) && CPU(ARM64)

#include "AOTFaultTable.h"
#include "AOTSavedFrame.h"
#include "AOTSlotRepresentation.h"
#include "AOTStubBodyConvention.h"
#include "AssemblerCommon.h"
#include "JSCJSValue.h"
#include <array>
#include <bit>
#include <limits>

namespace JSC { namespace AOT { namespace Machine {

template<typename... RegisterIDs>
static constexpr RegisterMask arm64TargetMaskOf(RegisterIDs... registers)
{
    return (RegisterMask { 0 } | ... | maskOf(Reg(registers)));
}

template<typename RegisterID>
static constexpr RegisterMask arm64TargetMaskOfRange(RegisterID first, RegisterID last)
{
    RegisterMask mask = 0;
    for (int number = first; number <= last; ++number)
        mask |= maskOf(Reg(static_cast<RegisterID>(number)));
    return mask;
}

template<typename RegisterID, size_t size>
static constexpr RegisterMask arm64TargetMaskOfList(const std::array<RegisterID, size>& registers)
{
    RegisterMask mask = 0;
    for (RegisterID reg : registers)
        mask |= maskOf(Reg(reg));
    return mask;
}

static constexpr RegisterMask arm64TargetPinnedRegisters = arm64TargetMaskOf(ARM64Registers::x18, cellCageBaseGPR, instanceGPR, GPRInfo::numberTagRegister, GPRInfo::notCellMaskRegister);
static constexpr RegisterMask arm64TargetScratchRegisters = arm64TargetMaskOf(scratchGPR, secondScratchGPR, scratchFPR);
static constexpr RegisterMask arm64TargetFrameRegisters = arm64TargetMaskOf(MacroAssembler::framePointerRegister, MacroAssembler::linkRegister, MacroAssembler::stackPointerRegister);
static constexpr RegisterMask arm64TargetCalleeSavedGPRs = arm64TargetMaskOfRange(ARM64Registers::x19, ARM64Registers::x24);
static constexpr RegisterMask arm64TargetCalleeSavedFPRs = arm64TargetMaskOfRange(ARM64Registers::q8, ARM64Registers::q15);
static constexpr RegisterMask arm64TargetAllocatableGPRs = arm64TargetMaskOfRange(ARM64Registers::x0, ARM64Registers::x15) | arm64TargetCalleeSavedGPRs;
static constexpr RegisterMask arm64TargetAllocatableFPRs = arm64TargetMaskOfRange(ARM64Registers::q0, ARM64Registers::q30);
static constexpr RegisterMask arm64TargetGPRsClobberedAtSuspensions = arm64TargetMaskOfList(suspensionClobberedGPRs);

static_assert(arm64TargetPinnedRegisters == (arm64TargetMaskOf(ARM64Registers::x18) | arm64TargetMaskOfRange(ARM64Registers::x25, ARM64Registers::x28)));
static_assert(arm64TargetScratchRegisters == arm64TargetMaskOf(ARM64Registers::x16, ARM64Registers::x17, ARM64Registers::q31));
static_assert(arm64TargetMaskOfList(allocatableGPRs) == arm64TargetAllocatableGPRs && scratchImmediateGPR == secondScratchGPR);
static_assert((arm64TargetAllocatableGPRs | arm64TargetAllocatableFPRs | arm64TargetPinnedRegisters | arm64TargetScratchRegisters | arm64TargetFrameRegisters) == ~RegisterMask { 0 });
static_assert(std::popcount(arm64TargetAllocatableGPRs) + std::popcount(arm64TargetAllocatableFPRs) + std::popcount(arm64TargetPinnedRegisters) + std::popcount(arm64TargetScratchRegisters) + std::popcount(arm64TargetFrameRegisters) == 64);
static_assert(!(arm64TargetCalleeSavedFPRs & ~arm64TargetAllocatableFPRs));
static_assert(arm64TargetMaskOfList(pinnedFPRsInFrameSavingFunctions) == arm64TargetCalleeSavedFPRs);
static_assert(SavedFrame::preservedRegister == InvalidGPRReg && arm64TargetGPRsClobberedAtSuspensions == arm64TargetCalleeSavedGPRs);
static_assert(unsavedCalleeSaveGPRs.empty());
static_assert(registerWrittenAtFaults == scratchGPR);
static_assert(!static_cast<unsigned>(Bank::GP) && static_cast<unsigned>(Bank::FP) == 1);

struct ARM64TargetPinnedConstant {
    uint64_t bits;
    GPRReg reg;
};

static constexpr std::array<ARM64TargetPinnedConstant, 2> arm64TargetPinnedConstants { {
    { static_cast<uint64_t>(JSValue::NumberTag), GPRInfo::numberTagRegister },
    { static_cast<uint64_t>(JSValue::NotCellMask), GPRInfo::notCellMaskRegister },
} };

static constexpr std::array<uint64_t, arm64TargetPinnedConstants.size()> arm64TargetBitsOfPinnedConstants = [] {
    std::array<uint64_t, arm64TargetPinnedConstants.size()> bits { };
    for (size_t index = 0; index < bits.size(); ++index)
        bits[index] = arm64TargetPinnedConstants[index].bits;
    return bits;
}();

static_assert([] {
    for (const ARM64TargetPinnedConstant& constant : arm64TargetPinnedConstants) {
        if (!(arm64TargetPinnedRegisters & maskOf(Reg(constant.reg))))
            return false;
    }
    return true;
}());

static_assert(JSValue::ValueNull < JSValue::ValueUndefined && JSValue::ValueUndefined == undefinedCell32, "a trapping access faults for these two bases, and for what these two words decode to");
static_assert((undefinedCell32 << cell32Shift) < FaultSite::reach);
static constexpr uint32_t arm64TargetTrapReach = static_cast<uint32_t>(FaultSite::reach - (undefinedCell32 << cell32Shift));

static constexpr int64_t arm64TargetMaxDistanceFromFrameAddress = int64_t { 1 } << 30;

static constexpr bool fitsARM64TargetOffset(int64_t value)
{
    return value == static_cast<int32_t>(value);
}

static constexpr bool isARM64TargetArithmeticImmediate(int64_t value)
{
    uint64_t magnitude = value < 0 ? uint64_t { 0 } - static_cast<uint64_t>(value) : static_cast<uint64_t>(value);
    return !(magnitude & ~uint64_t { 0xfff }) || !(magnitude & ~uint64_t { 0xfff000 });
}

static_assert(isARM64TargetArithmeticImmediate(0xfff) && isARM64TargetArithmeticImmediate(-0xfff) && isARM64TargetArithmeticImmediate(0xfff000) && isARM64TargetArithmeticImmediate(-0xfff000));
static_assert(!isARM64TargetArithmeticImmediate(0x1001) && !isARM64TargetArithmeticImmediate(0x1000000) && !isARM64TargetArithmeticImmediate(std::numeric_limits<int64_t>::min()));

static constexpr bool isARM64TargetArithmeticMagnitude(uint64_t magnitude)
{
    return !(magnitude & ~uint64_t { 0xfff }) || !(magnitude & ~uint64_t { 0xfff000 });
}

std::optional<ConstantComparison> findConstantComparison(Opcode relation, Width width, int64_t constant)
{
    ASSERT(width == Width32 || width == Width64);
    if (isARM64TargetArithmeticImmediate(constant))
        return ConstantComparison { ConstantComparison::Immediate, relation, constant };

    bool is32 = width == Width32;
    if (relation == Above || relation == BelowEqual || relation == AboveEqual || relation == Below) {
        uint64_t greatest = is32 ? UINT32_MAX : UINT64_MAX;
        uint64_t bound = is32 ? static_cast<uint32_t>(constant) : static_cast<uint64_t>(constant);
        if (relation == Above || relation == BelowEqual) {
            if (bound == greatest)
                return std::nullopt;
            ++bound;
            relation = relation == Above ? AboveEqual : Below;
            int64_t canonical = is32 ? static_cast<int32_t>(bound) : static_cast<int64_t>(bound);
            if (bound && isARM64TargetArithmeticImmediate(canonical))
                return ConstantComparison { ConstantComparison::Immediate, relation, canonical };
        }
        if (!std::has_single_bit(bound))
            return std::nullopt;
        return ConstantComparison { ConstantComparison::HighBits, relation, 0, static_cast<unsigned>(std::countr_zero(bound)) };
    }

    if (relation != LessThan && relation != LessEqual && relation != GreaterThan && relation != GreaterEqual)
        return std::nullopt;
    int64_t least = is32 ? std::numeric_limits<int32_t>::min() : std::numeric_limits<int64_t>::min();
    int64_t greatest = is32 ? std::numeric_limits<int32_t>::max() : std::numeric_limits<int64_t>::max();
    ASSERT(constant >= least && constant <= greatest);
    if (relation == LessEqual || relation == GreaterThan) {
        if (constant == greatest)
            return std::nullopt;
        ++constant;
        relation = relation == LessEqual ? LessThan : GreaterEqual;
        if (isARM64TargetArithmeticImmediate(constant))
            return ConstantComparison { ConstantComparison::Immediate, relation, constant };
    }

    uint64_t distanceFromLeast = static_cast<uint64_t>(constant) - static_cast<uint64_t>(least);
    if (distanceFromLeast && isARM64TargetArithmeticMagnitude(distanceFromLeast))
        return ConstantComparison { ConstantComparison::Overflow, relation, static_cast<int64_t>(distanceFromLeast), 0, relation == LessThan };
    uint64_t distanceToGreatest = static_cast<uint64_t>(greatest) - static_cast<uint64_t>(constant) + 1;
    if (isARM64TargetArithmeticMagnitude(distanceToGreatest))
        return ConstantComparison { ConstantComparison::Overflow, relation, -static_cast<int64_t>(distanceToGreatest), 0, relation == GreaterEqual };
    if (constant > 0 && std::has_single_bit(static_cast<uint64_t>(constant)))
        return ConstantComparison { ConstantComparison::HighBits, relation, 0, static_cast<unsigned>(std::countr_zero(static_cast<uint64_t>(constant))) };
    return std::nullopt;
}

static bool isARM64TargetZeroNumber(const Operation& constant)
{
    if (constant.hasDouble())
        return !constant.asDouble();
    return constant.hasFloat() && !constant.asFloat();
}

static bool arm64TargetAddressAcceptsBase(const Operation& access, const Operation& base)
{
    if (access.traps())
        return false;

    int64_t distance = access.offset();
    if (base.opcode() == InstanceAddressOf)
        return !distance;

    const Operation* start = &base;
    if (base.opcode() == Add) {
        int64_t addend = base.child(1)->asInt64();
        if (!fitsARM64TargetOffset(addend))
            return false;
        distance += addend;
        start = base.child(0);
        if (start->opcode() == ArgumentReg)
            return fitsARM64TargetOffset(distance);
    }
    if (start->opcode() != FramePointer && start->opcode() != SlotBase)
        return false;
    return distance >= -arm64TargetMaxDistanceFromFrameAddress && distance <= arm64TargetMaxDistanceFromFrameAddress;
}

RegisterFile registerFileFor(const ProcedureTraits& traits)
{
    if (traits.savesFrameOnSuspend) {
        return {
            .allocatable = { arm64TargetAllocatableGPRs, arm64TargetAllocatableFPRs & ~arm64TargetCalleeSavedFPRs },
            .calleeSaved = arm64TargetCalleeSavedGPRs,
            .calleeSavedClobberedAtSuspensions = arm64TargetGPRsClobberedAtSuspensions,
        };
    }
    if (traits.savesRegistersOfCallers) {
        RegisterMask allocatableGPRs = arm64TargetAllocatableGPRs & ~arm64TargetCalleeSavedGPRs;
        RegisterMask allocatableFPRs = arm64TargetAllocatableFPRs & ~arm64TargetCalleeSavedFPRs;
        return {
            .allocatable = { allocatableGPRs, allocatableFPRs },
            .calleeSaved = (allocatableGPRs | allocatableFPRs) & maskOf(StubBodyConvention::registersSavedByCallee()),
            .calleeSavedClobberedAtSuspensions = 0,
        };
    }
    return {
        .allocatable = { arm64TargetAllocatableGPRs, arm64TargetAllocatableFPRs },
        .calleeSaved = arm64TargetCalleeSavedGPRs | arm64TargetCalleeSavedFPRs,
        .calleeSavedClobberedAtSuspensions = 0,
    };
}

RegisterMask pinnedRegisters()
{
    return arm64TargetPinnedRegisters;
}

std::optional<PinnedConstant> findPinnedConstant(uint64_t bits)
{
    for (const ARM64TargetPinnedConstant& constant : arm64TargetPinnedConstants) {
        if (constant.bits == bits)
            return PinnedConstant { .reg = constant.reg, .offset = std::nullopt, .replacesImmediates = false };
    }
    return std::nullopt;
}

std::span<const uint64_t> bitsOfPinnedConstants()
{
    return arm64TargetBitsOfPinnedConstants;
}

bool acceptsImmediate(const Operation& user, unsigned childIndex, const Operation& constant)
{
    if (!constant.hasNoPosition())
        return false;

    switch (user.opcode()) {
    case Add:
    case Sub:
        return childIndex == 1 && constant.hasInt() && isARM64TargetArithmeticImmediate(constant.asInt());

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
        if (childIndex != 1)
            return false;
        if (constant.hasInt())
            return !!findConstantComparison(user.opcode(), widthOf(constant.type()), constant.asInt());
        return isARM64TargetZeroNumber(constant);

    case BitAnd:
    case BitOr:
        return childIndex == 1 && constant.hasInt() && isEncodableBitImmediate(widthOf(user.type()), constant.asInt());

    case BitXor:
        return childIndex == 1 && constant.hasInt() && (constant.isInt(-1) || isEncodableBitImmediate(widthOf(user.type()), constant.asInt()));

    case Shl:
    case SShr:
    case ZShr:
    case RotR:
        return childIndex == 1 && constant.hasInt32();

    case Load8Z:
    case Load8S:
    case Load16Z:
    case Load16S:
    case Load:
        return arm64TargetAddressAcceptsBase(user, constant);

    case Store8:
    case Store16:
    case Store:
        if (childIndex)
            return arm64TargetAddressAcceptsBase(user, constant);
        return constant.isConstant() && !(static_cast<uint64_t>(constant.constantBits()) & JSC::mask(user.accessWidth()));

    case Check:
        return !childIndex && constant.hasInt();

    case Opaque:
        return user.type().isInt() && constant.opcode() != SlotBase;

    default:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool isEncodableBitImmediate(Width width, int64_t bits)
{
    switch (width) {
    case Width32:
        return ARM64LogicalImmediate::create32(static_cast<uint32_t>(bits)).isValid();
    case Width64:
        return ARM64LogicalImmediate::create64(static_cast<uint64_t>(bits)).isValid();
    case Width8:
    case Width16:
    case Width128:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool acceptsMemoryOperand(const Operation&, unsigned)
{
    return false;
}

bool acceptsMemoryDestination(const Operation&)
{
    return false;
}

bool isEncodableArithmeticConstant(int64_t constant)
{
    return isARM64TargetArithmeticImmediate(constant);
}

bool isEncodableIndexedAddress(Width accessWidth, unsigned shift, IndexExtension, int32_t offset)
{
    if (accessWidth == Width128)
        return !offset && !shift;
    return !offset && (!shift || shift == static_cast<unsigned>(std::countr_zero(bytesForWidth(accessWidth))));
}

bool isEncodableScaledSum(unsigned shift, IndexExtension extension)
{
    return shift >= 1 && shift <= (extension == IndexExtension::None ? 63u : 4u);
}

bool scaledSumAcceptsBase(const Operation&)
{
    return false;
}

unsigned numberOfScratchGPRsOf(const ProcedureTraits&, const Operation& operation)
{
    if (operation.opcode() != Switch)
        return 0;
    return operation.switchData().caseValues.size() >= minCasesForSwitchWindow ? 1 : 0;
}

uint32_t trapReach()
{
    return arm64TargetTrapReach;
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT) && CPU(ARM64)
