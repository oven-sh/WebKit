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
// Every register has one role.
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

// Offsets within the frame are not known until after register allocation. This bound leaves 2^30 bytes of the int32 offset for them, more than any frame needs.
static constexpr int64_t arm64TargetMaxDistanceFromFrameAddress = int64_t { 1 } << 30;

static constexpr bool fitsARM64TargetOffset(int64_t value)
{
    return value == static_cast<int32_t>(value);
}

// A 12-bit immediate, optionally shifted left by 12. MacroAssembler encodes a negative one with the opposite instruction: sub for add, cmn for cmp.
static constexpr bool isARM64TargetArithmeticImmediate(int64_t value)
{
    uint64_t magnitude = value < 0 ? uint64_t { 0 } - static_cast<uint64_t>(value) : static_cast<uint64_t>(value);
    return !(magnitude & ~uint64_t { 0xfff }) || !(magnitude & ~uint64_t { 0xfff000 });
}

static_assert(isARM64TargetArithmeticImmediate(0xfff) && isARM64TargetArithmeticImmediate(-0xfff) && isARM64TargetArithmeticImmediate(0xfff000) && isARM64TargetArithmeticImmediate(-0xfff000));
static_assert(!isARM64TargetArithmeticImmediate(0x1001) && !isARM64TargetArithmeticImmediate(0x1000000) && !isARM64TargetArithmeticImmediate(std::numeric_limits<int64_t>::min()));

// -0.0 counts too: fcmp has a form that compares with 0.0, and no comparison tells the two zeros apart.
static bool isARM64TargetZeroNumber(const Operation& constant)
{
    if (constant.hasDouble())
        return !constant.asDouble();
    return constant.hasFloat() && !constant.asFloat();
}

static bool arm64TargetAddressAcceptsBase(const Operation& access, const Operation& base)
{
    // The record of a trapping access names the register that holds the tested word, so its base has to be in one.
    if (access.traps())
        return false;

    int64_t distance = access.offset();
    if (base.opcode() == InstanceAddressOf)
        return !distance;

    // Builder::load() and Builder::store() fold a constant addend into the offset whenever the sum fits in an int32, so the sum of an Add that is still under an access may not fit.
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

// Must match how Encoder::encodeSwitch() finds its jump tables: a run keeps growing while its values are dense: maxEntriesOfJumpTablePerCase.
static bool arm64TargetSwitchHasDenseRun(std::span<const int64_t> values)
{
    for (size_t begin = 0; begin + minCasesForJumpTable <= values.size(); ++begin) {
        size_t numberOfCases = 1;
        for (; numberOfCases < minCasesForJumpTable; ++numberOfCases) {
            uint64_t distanceToNext = static_cast<uint64_t>(values[begin + numberOfCases]) - static_cast<uint64_t>(values[begin]);
            if (distanceToNext >= maxEntriesOfJumpTablePerCase * (numberOfCases + 1))
                break;
        }
        if (numberOfCases == minCasesForJumpTable)
            return true;
    }
    return false;
}

RegisterFile registerFileFor(const ProcedureTraits& traits)
{
    // A saved frame has no room for FPRs, and the caller expects q8 to q15 to be preserved, so such a function never writes them.
    if (traits.savesFrameOnSuspend) {
        return {
            .allocatable = { arm64TargetAllocatableGPRs, arm64TargetAllocatableFPRs & ~arm64TargetCalleeSavedFPRs },
            .calleeSaved = arm64TargetCalleeSavedGPRs,
            .calleeSavedClobberedAtSuspensions = arm64TargetGPRsClobberedAtSuspensions,
        };
    }
    // An ImageFrame lists what unwinding restores as one run of slots, among which such a function would save other registers. So it writes none of x19 to x24 and q8 to q15.
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
            return isARM64TargetArithmeticImmediate(constant.asInt());
        return isARM64TargetZeroNumber(constant);

    case BitAnd:
    case BitOr:
        return childIndex == 1 && constant.hasInt() && isEncodableBitImmediate(widthOf(user.type()), constant.asInt());

    // Xor with all ones is mvn.
    case BitXor:
        return childIndex == 1 && constant.hasInt() && (constant.isInt(-1) || isEncodableBitImmediate(widthOf(user.type()), constant.asInt()));

    case Shl:
    case SShr:
    case ZShr:
        return childIndex == 1 && constant.hasInt32();

    case Load8Z:
    case Load8S:
    case Load16Z:
    case Load16S:
    case Load:
        return arm64TargetAddressAcceptsBase(user, constant);

    // Zero is stored from zr.
    case Store8:
    case Store16:
    case Store:
        if (childIndex)
            return arm64TargetAddressAcceptsBase(user, constant);
        return constant.isConstant() && !(static_cast<uint64_t>(constant.constantBits()) & JSC::mask(user.accessWidth()));

    // Encoder::encodeCheck() turns a constant predicate into a jump or into nothing, and never reads its location.
    case Check:
        return !childIndex && constant.hasInt();

    // The encoder makes the constant in the register of the result. Allocator::placeLeaf() answers a SlotBase with the bytes of the slot, which is no place to take an address from.
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
    return arm64TargetSwitchHasDenseRun(operation.switchData().caseValues) ? 1 : 0;
}

uint32_t trapReach()
{
    return arm64TargetTrapReach;
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT) && CPU(ARM64)
