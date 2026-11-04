/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachine.h"
#include "AOTStubRegisters.h"

// What the scan, the allocator and the encoders have to agree on about the instruction set. AOTMachineTargetARM64.cpp and AOTMachineTargetX86_64.cpp define it, each for its CPU().

namespace JSC { namespace AOT { namespace Machine {

// ARM64: x0 to x15 and x19 to x24, q0 to q30. x19 to x24 and q8 to q15 are saved by the callee. Pinned: x25 cage base, x26 instance, x27 and x28 tags, x18. Scratch: x16, x17, q31.
// x86-64: rax, rcx, rdx, rbx, rsi, rdi, r8, r9, r10, r12, r15, xmm0 to xmm14. rbx and r15 are saved by the callee, r12 is not. Pinned: r13 instance, r14 cage base. Scratch: r11, xmm15.
struct RegisterFile {
    RegisterMask allocatableIn(Bank bank) const { return allocatable[static_cast<unsigned>(bank)]; }
    RegisterMask calleeSavedIn(Bank bank) const { return allocatable[static_cast<unsigned>(bank)] & calleeSaved; }

    RegisterMask allocatable[numberOfBanks] { 0, 0 };
    // Among the allocatable ones: what the function saves before it writes it. In a function that saves its frame: no FPR.
    // With ProcedureTraits::savesRegistersOfCallers the registers above that callees save are not allocatable, and these are all the others but StubBodyConvention::registersClobberedByCalls().
    // A call that such a function makes may clobber them: whether a value keeps its register across a call is said by the clobber set of the call alone.
    RegisterMask calleeSaved { 0 };
    // Among the callee-saved ones: what a suspension does not keep.
    RegisterMask calleeSavedClobberedAtSuspensions { 0 };
};

JS_EXPORT_PRIVATE RegisterFile registerFileFor(const ProcedureTraits&);
JS_EXPORT_PRIVATE RegisterMask pinnedRegisters();
inline bool isPinned(Reg reg) { return pinnedRegisters() & maskOf(reg); }
constexpr Bank bankOf(Reg reg) { return reg.isGPR() ? Bank::GP : Bank::FP; }

// Dead at every operation and written by the handler of a fault. The parallel moves and the encoders use them, a generator only under AllowMacroScratchRegisterUsage.
#if CPU(ARM64)
inline constexpr GPRReg scratchGPR = MacroAssembler::dataTempRegister;
inline constexpr GPRReg secondScratchGPR = MacroAssembler::memoryTempRegister;
#elif CPU(X86_64)
inline constexpr GPRReg scratchGPR = MacroAssembler::s_scratchRegister;
inline constexpr GPRReg secondScratchGPR = InvalidGPRReg;
#endif
inline constexpr FPRReg scratchFPR = MacroAssembler::fpTempRegister;

// A constant that lies in a pinned register or in a word of the instance.
struct PinnedConstant {
    GPRReg reg;
    // The constant is the word at this offset from the register. Without one it is the register itself.
    std::optional<int32_t> offset;
    // Also where the constant would fit the instruction as an immediate.
    bool replacesImmediates { false };
};
JS_EXPORT_PRIVATE std::optional<PinnedConstant> findPinnedConstant(uint64_t bits);

// The bits of every constant that findPinnedConstant() knows.
JS_EXPORT_PRIVATE std::span<const uint64_t> bitsOfPinnedConstants();

// Whether the instruction of `user` takes this child without position in itself. Otherwise the allocator puts it into a register first.
// A shift takes every constant amount. A store takes zero on both, and on x86-64 what fits 32 bits with its sign.
// A load or a store with the offset zero takes an InstanceAddressOf as its base, and a SlotBase, the FramePointer or an Add without position wherever the whole distance from the register fits 32 bits.
JS_EXPORT_PRIVATE bool acceptsImmediate(const Operation& user, unsigned childIndex, const Operation& constant);

// Whether an and, an or, an exclusive or or a test of that width takes the bits as an immediate. ARM64: a logical immediate. x86-64: what fits 32 bits with its sign.
JS_EXPORT_PRIVATE bool isEncodableBitImmediate(Width, int64_t bits);

// x86-64: whether the instruction of `user` reads this child from memory: a spilled value in its slot, a pinned constant in its word, a fused load at its address. At most one child of an operation does.
JS_EXPORT_PRIVATE bool acceptsMemoryOperand(const Operation& user, unsigned childIndex);

// x86-64: whether the instruction of the operation takes a word in memory for one operand and for its result at once: MemoryUpdate.
JS_EXPORT_PRIVATE bool acceptsMemoryDestination(const Operation& computation);

// ARM64 has one form of a MemoryUpdate: TestedBehind where overflow is tested, with or without a least value. The word goes through the scratch register, the flags of the addition are read, and the word is stored behind the tests.
// MacroAssembler may make an immediate of more than twelve bits in that very register.
constexpr bool updatesMemoryThroughScratchRegister() { return isARM64(); }
inline constexpr int64_t maxConstantAddedInScratchRegister = 0xfff;

// Whether an addition, a subtraction or a comparison of 64 bits takes the constant without a register of its own: as an immediate, or on x86-64 from its word of the instance.
JS_EXPORT_PRIVATE bool isEncodableArithmeticConstant(int64_t);

enum class IndexExtension : uint8_t { None, ZeroExtend32, SignExtend32 };

// Whether one instruction reaches [base + (extended index << shift) + offset]. Without an index every offset is reached.
JS_EXPORT_PRIVATE bool isEncodableIndexedAddress(Width accessWidth, unsigned shift, IndexExtension, int32_t offset);

// Whether one instruction puts base + (extended index << shift) into a register. x86-64: lea, by one, two or three. ARM64: add, of a shifted register by any amount, of an extended one by up to four.
JS_EXPORT_PRIVATE bool isEncodableScaledSum(unsigned shift, IndexExtension);
// Whether that instruction takes this base without position in itself. x86-64: a pinned register with a constant of 32 bits, which is the displacement.
JS_EXPORT_PRIVATE bool scaledSumAcceptsBase(const Operation& base);

// The result is cheapest in the register of the first child: the instruction overwrites it.
constexpr bool resultPrefersRegisterOfFirstChild() { return isX86_64(); }
// Where that holds: the operations whose instruction does so.
constexpr bool resultOverwritesFirstChild(Opcode opcode)
{
    return opcode == Add || opcode == Sub || opcode == Mul || opcode == Div || opcode == Neg
        || opcode == BitAnd || opcode == BitOr || opcode == BitXor || opcode == Shl || opcode == SShr || opcode == ZShr;
}

// Of an operation with an Int32 result: the last instruction that its encoder writes is one that writes 32 bits of the register, which clears the upper half on both targets.
// Every step that moves an Int32 keeps the upper half clear: a Move and a Reload of 32 bits clear it again, a Swap takes the whole register along.
// A NumberOf is made by an instruction that writes the number to the low half and clears the rest.
// Not among them: a Trunc and an Opaque, which copy a register or are no instruction at all, a Select, which copies one of two registers whole, MulHigh, whose shift on ARM64 leaves the sign there,
// a Phi, which may be any of these, and what a generator or a caller has written.
// Whoever has the operation asks the second form, which knows more of a Phi.
constexpr bool clearsUpperHalfOfRegister(Opcode opcode)
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
    case Nop:
    case Opaque:
    case Const32:
    case Const64:
    case ConstFloat:
    case ConstDouble:
    case FramePointer:
    case SlotBase:
    case AddressOf:
    case InstanceAddressOf:
    case ArgumentReg:
    case MulHigh:
    case UMulHigh:
    case Mod:
    case UMod:
    case Abs:
    case Ceil:
    case Floor:
    case FTrunc:
    case Sqrt:
    case FMin:
    case FMax:
    case BitwiseCast:
    case SExt32:
    case ZExt32:
    case Trunc:
    case IToD:
    case FloatToDouble:
    case DoubleToFloat:
    case Select:
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
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

// What an Upsilon may hand to a Phi of 32 bits whose register is to have a clear upper half: the moves of the edge keep it clear, and a constant that is not negative is made with it clear whichever way it is made.
// The Phi itself comes round as it is. Another Phi is not asked.
inline bool handsOnClearUpperHalf(const Operation* value, const Operation* phi)
{
    return value == phi || (value->hasInt32() ? value->asInt32() >= 0 : clearsUpperHalfOfRegister(value->opcode()));
}

inline bool clearsUpperHalfOfRegister(const Operation* value)
{
    return value->opcode() == Phi ? !value->has(OperationFlag::UpperHalfMayBeSet) : clearsUpperHalfOfRegister(value->opcode());
}

// Registers beside the scratch registers above that the encoder needs for the operation. They differ from those of its operands and its result.
JS_EXPORT_PRIVATE unsigned numberOfScratchGPRsOf(const ProcedureTraits&, const Operation&);

// From how many cases on a Switch whose values are dense goes through a table in the constants.
inline constexpr unsigned minCasesForJumpTable = 4;
// Values are dense while the range that they cover has at most so many entries for each of them. `switch (node.kind)` names some twenty of two hundred kinds: a search makes five to nine compares, the table one.
inline constexpr uint64_t maxEntriesOfJumpTablePerCase = 16;

// A trapping access faults for the bases null and undefined and for what their narrow words decode to, if its offset is below this.
JS_EXPORT_PRIVATE uint32_t trapReach();

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
