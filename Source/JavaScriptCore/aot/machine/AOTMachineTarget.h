/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachine.h"
#include "AOTStubRegisters.h"

namespace JSC { namespace AOT { namespace Machine {

struct RegisterFile {
    RegisterMask allocatableIn(Bank bank) const { return allocatable[static_cast<unsigned>(bank)]; }
    RegisterMask calleeSavedIn(Bank bank) const { return allocatable[static_cast<unsigned>(bank)] & calleeSaved; }

    RegisterMask allocatable[numberOfBanks] { 0, 0 };
    RegisterMask calleeSaved { 0 };
    RegisterMask calleeSavedClobberedAtSuspensions { 0 };
};

JS_EXPORT_PRIVATE RegisterFile registerFileFor(const ProcedureTraits&);
#if CPU(X86_64)
constexpr RegisterMask pinnedRegisters() { return maskOf(Reg(instanceGPR)) | maskOf(Reg(cellCageBaseGPR)); }
#else
JS_EXPORT_PRIVATE RegisterMask pinnedRegisters();
#endif
inline bool isPinned(Reg reg) { return pinnedRegisters() & maskOf(reg); }
constexpr Bank bankOf(Reg reg) { return reg.isGPR() ? Bank::GP : Bank::FP; }

#if CPU(ARM64)
inline constexpr GPRReg scratchGPR = MacroAssembler::dataTempRegister;
inline constexpr GPRReg secondScratchGPR = MacroAssembler::memoryTempRegister;
#elif CPU(X86_64)
inline constexpr GPRReg scratchGPR = MacroAssembler::s_scratchRegister;
inline constexpr GPRReg secondScratchGPR = InvalidGPRReg;
#endif
inline constexpr FPRReg scratchFPR = MacroAssembler::fpTempRegister;

struct PinnedConstant {
    GPRReg reg;
    std::optional<int32_t> offset;
    bool replacesImmediates { false };
};
JS_EXPORT_PRIVATE std::optional<PinnedConstant> findPinnedConstant(uint64_t bits);

JS_EXPORT_PRIVATE std::span<const uint64_t> bitsOfPinnedConstants();

#if CPU(X86_64)
inline constexpr int64_t x86TargetMaxDistanceFromFrameAddress = int64_t { 1 } << 30;

constexpr bool fitsX86TargetImmediate(int64_t value)
{
    return value == static_cast<int32_t>(value);
}

inline bool isX86TargetImmediate(const Operation& constant)
{
    return constant.hasInt() && fitsX86TargetImmediate(constant.asInt());
}

inline bool x86TargetAddressAcceptsBase(const Operation& access, const Operation& base)
{
    int64_t distance = access.offset();
    if (base.opcode() == InstanceAddressOf)
        return !distance;

    const Operation* start = &base;
    if (base.opcode() == Add) {
        int64_t addend = base.child(1)->asInt64();
        if (!fitsX86TargetImmediate(addend))
            return false;
        distance += addend;
        start = base.child(0);
        if (start->opcode() == ArgumentReg)
            return fitsX86TargetImmediate(distance);
    }
    if (start->opcode() != FramePointer && start->opcode() != SlotBase)
        return false;
    return distance >= -x86TargetMaxDistanceFromFrameAddress && distance <= x86TargetMaxDistanceFromFrameAddress;
}

ALWAYS_INLINE bool readsConstantInMemory(const Operation& user, unsigned childIndex, const Operation& constant)
{
    if (childIndex != 1)
        return false;
    Opcode opcode = user.opcode();
    if (constant.hasInt())
        return isComparison(opcode) && opcode != EqualOrUnordered && !isX86TargetImmediate(constant);
    return constant.hasDouble() && constant.constantBits() >> 32 && (opcode == Add || opcode == Sub || opcode == Mul || opcode == Div);
}

ALWAYS_INLINE bool acceptsImmediate(const Operation& user, unsigned childIndex, const Operation& constant)
{
    if (!constant.hasNoPosition())
        return false;

    switch (user.opcode()) {
    case Add:
    case Sub:
    case Mul:
    case BitAnd:
    case BitOr:
    case BitXor:
    case LessThan:
    case GreaterThan:
    case LessEqual:
    case GreaterEqual:
    case Above:
    case Below:
    case AboveEqual:
    case BelowEqual:
    case EqualOrUnordered:
        return childIndex == 1 && isX86TargetImmediate(constant);

    case Equal:
    case NotEqual:
        return childIndex == 1 && (constant.opcode() == NumberOf || isX86TargetImmediate(constant));

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
        return x86TargetAddressAcceptsBase(user, constant);

    case Store8:
    case Store16:
    case Store:
        if (childIndex)
            return x86TargetAddressAcceptsBase(user, constant);
        return isX86TargetImmediate(constant);

    case Check:
        return !childIndex && constant.hasInt();

    case Opaque:
        return user.type().isInt() && constant.opcode() != SlotBase;

    case Nop:
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
    case MulHigh:
    case UMulHigh:
    case Div:
    case UDiv:
    case Mod:
    case UMod:
    case Neg:
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
    case Select:
    case Fence:
    case Phi:
    case Upsilon:
    case Extract:
    case Patch:
    case Jump:
    case Branch:
    case Switch:
    case Return:
    case Oops:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}
#else
constexpr bool readsConstantInMemory(const Operation&, unsigned, const Operation&) { return false; }
JS_EXPORT_PRIVATE bool acceptsImmediate(const Operation& user, unsigned childIndex, const Operation& constant);
#endif

JS_EXPORT_PRIVATE bool isEncodableBitImmediate(Width, int64_t bits);

#if CPU(ARM64)
struct ConstantComparison {
    enum Kind : uint8_t {
        Immediate,
        Overflow,
        HighBits,
    };

    Kind kind;
    Opcode relation;
    int64_t constant;
    unsigned shift { 0 };
    bool holdsOnOverflow { false };
};
std::optional<ConstantComparison> findConstantComparison(Opcode relation, Width, int64_t constant);
#endif

#if CPU(X86_64)
ALWAYS_INLINE bool acceptsMemoryOperand(const Operation& user, unsigned childIndex)
{
    switch (user.opcode()) {
    case Add:
    case Sub:
    case Mul:
    case BitOr:
    case BitXor:
        return childIndex == 1;

    case Div:
        return childIndex == 1 && user.type().isFloat();

    case BitAnd:
        return true;

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
        return childIndex == 1 || user.child(0)->type().isInt();

    case Branch:
    case Select:
    case Check:
    case Sqrt:
    case BitwiseCast:
    case SExt8:
    case SExt16:
    case SExt32:
    case ZExt32:
    case IToD:
    case FloatToDouble:
    case DoubleToFloat:
        return !childIndex;

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
    case MulHigh:
    case UMulHigh:
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
    case FMin:
    case FMax:
    case Trunc:
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
    case Jump:
    case Switch:
    case Return:
    case Oops:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}
#else
JS_EXPORT_PRIVATE bool acceptsMemoryOperand(const Operation& user, unsigned childIndex);
#endif

JS_EXPORT_PRIVATE bool acceptsMemoryDestination(const Operation& computation);

constexpr bool updatesMemoryThroughScratchRegister() { return isARM64(); }
inline constexpr int64_t maxConstantAddedInScratchRegister = 0xfff;

JS_EXPORT_PRIVATE bool isEncodableArithmeticConstant(int64_t);

enum class IndexExtension : uint8_t { None, ZeroExtend32, SignExtend32 };

JS_EXPORT_PRIVATE bool isEncodableIndexedAddress(Width accessWidth, unsigned shift, IndexExtension, int32_t offset);

JS_EXPORT_PRIVATE bool isEncodableScaledSum(unsigned shift, IndexExtension);
JS_EXPORT_PRIVATE bool scaledSumAcceptsBase(const Operation& base);

constexpr bool acceptsShiftedOperand(Opcode opcode)
{
    return isARM64() && (opcode == Add || opcode == Sub || opcode == BitAnd || opcode == BitOr || opcode == BitXor);
}

constexpr bool acceptsExtendedOperand(Opcode opcode) { return isARM64() && (opcode == Add || opcode == Sub || isComparison(opcode)); }

constexpr bool resultPrefersRegisterOfFirstChild() { return isX86_64(); }
constexpr bool resultOverwritesFirstChild(Opcode opcode)
{
    return opcode == Add || opcode == Sub || opcode == Mul || opcode == Div || opcode == Neg
        || opcode == BitAnd || opcode == BitOr || opcode == BitXor || opcode == Shl || opcode == SShr || opcode == ZShr || opcode == RotR;
}

inline bool hasThreeAddressFormWithConstant(const Operation& operation)
{
    switch (operation.opcode()) {
    case Add:
    case Mul:
        return true;
    case BitAnd:
        return operation.child(1)->isInt(0xff) || operation.child(1)->isInt(0xffff);
    case Shl:
        return operation.child(1)->isInt(1);
    default:
        return false;
    }
}

constexpr bool clearsUpperHalf(Opcode opcode)
{
    switch (opcode) {
    case Add:
    case Sub:
    case Mul:
    case Div:
    case UDiv:
    case Neg:
    case BitAnd:
    case BitOr:
    case BitXor:
    case Shl:
    case SShr:
    case ZShr:
    case RotR:
    case Clz:
    case PopCount:
    case SExt8:
    case SExt16:
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
    case Load8Z:
    case Load8S:
    case Load16Z:
    case Load16S:
    case Load:
    case NumberOf:
        return true;
    default:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

inline bool handsOnClearUpperHalf(const Operation* value)
{
    if (value->opcode() == Patch)
        return static_cast<const PatchOperation*>(value)->clearsUpperHalf();
    return value->hasInt32() ? value->asInt32() >= 0 : clearsUpperHalf(value->opcode());
}

ALWAYS_INLINE void Procedure::noteUpperHalfHandedOn(Operation* upsilon)
{
    const Operation* value = upsilon->child(0);
    Operation* phi = upsilon->phi();
    if (value->opcode() == Phi) {
        if (value != phi && value->type() == Int32)
            m_upsilonsOfWordsBetweenPhis.append(m_arena, upsilon);
    } else if (!handsOnClearUpperHalf(value))
        phi->m_flags |= static_cast<uint16_t>(OperationFlag::UpperHalfMayBeSet);
}

inline bool clearsUpperHalf(const Operation* value)
{
    if (value->opcode() == Phi)
        return !value->has(OperationFlag::UpperHalfMayBeSet);
    return value->opcode() == Patch ? static_cast<const PatchOperation*>(value)->clearsUpperHalf() : clearsUpperHalf(value->opcode());
}

ALWAYS_INLINE void Procedure::noteWordName(Operation* extension)
{
    if (extension->opcode() == Trunc || clearsUpperHalf(extension->child(0)))
        m_namesOfWords.append(m_arena, extension);
}

JS_EXPORT_PRIVATE unsigned numberOfScratchGPRsOf(const ProcedureTraits&, const Operation&);

inline constexpr unsigned minCasesForJumpTable = 7;
inline constexpr uint64_t maxEntriesOfJumpTablePerCase = 4;
inline constexpr unsigned minCasesForSwitchWindow = 4;
inline constexpr uint64_t valuesInSwitchWindow = 64;
inline constexpr unsigned maxPlacesToldByBitsOfSwitchWindow = 3;

JS_EXPORT_PRIVATE uint32_t trapReach();

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
