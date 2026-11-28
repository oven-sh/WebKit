/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineBuilder.h"

#if ENABLE(AOT) && CPU(X86_64)

namespace JSC { namespace AOT { namespace Machine {

template<Opcode opcode, Width width>
struct FixedRegisterDivisionGenerator {
    static_assert(opcode == Div || opcode == UDiv || opcode == Mod || opcode == UMod);
    static_assert(width == Width32 || width == Width64);

    void operator()(CCallHelpers& jit, const PatchParameters& parameters) const
    {
        GPRReg divisor = parameters[2].gpr();
        RELEASE_ASSERT(divisor != X86Registers::eax && divisor != X86Registers::edx);

        if constexpr (opcode == Div || opcode == Mod) {
            if constexpr (width == Width32) {
                jit.x86ConvertToDoubleWord32(X86Registers::eax, X86Registers::edx);
                jit.x86Div32(X86Registers::eax, X86Registers::edx, divisor);
            } else {
                jit.x86ConvertToQuadWord64(X86Registers::eax, X86Registers::edx);
                jit.x86Div64(X86Registers::eax, X86Registers::edx, divisor);
            }
        } else {
            jit.xor32(X86Registers::edx, X86Registers::edx);
            if constexpr (width == Width32)
                jit.x86UDiv32(X86Registers::eax, X86Registers::edx, divisor);
            else
                jit.x86UDiv64(X86Registers::eax, X86Registers::edx, divisor);
        }
    }
};

template<Opcode opcode, Width width>
struct FixedRegisterMultiplicationGenerator {
    static_assert(opcode == MulHigh || opcode == UMulHigh);
    static_assert(width == Width32 || width == Width64);

    void operator()(CCallHelpers& jit, const PatchParameters& parameters) const
    {
        GPRReg factor = parameters[1].gpr();

        if constexpr (opcode == MulHigh) {
            if constexpr (width == Width32)
                jit.x86MulHigh32(factor, X86Registers::eax, X86Registers::edx);
            else
                jit.x86MulHigh64(factor, X86Registers::eax, X86Registers::edx);
        } else {
            if constexpr (width == Width32)
                jit.x86UMulHigh32(factor, X86Registers::eax, X86Registers::edx);
            else
                jit.x86UMulHigh64(factor, X86Registers::eax, X86Registers::edx);
        }
    }
};

struct FoldedProductGenerator {
    void operator()(CCallHelpers& jit, const PatchParameters& parameters) const
    {
        jit.x86UMulHigh64(parameters[1].gpr(), X86Registers::eax, X86Registers::edx);
        jit.xor32(X86Registers::edx, X86Registers::eax);
    }
};

template<Opcode opcode, Width width>
struct FixedRegisterShiftGenerator {
    static_assert(opcode == Shl || opcode == SShr || opcode == ZShr);
    static_assert(width == Width32 || width == Width64);

    void operator()(CCallHelpers& jit, const PatchParameters& parameters) const
    {
        GPRReg result = parameters[0].gpr();
        GPRReg value = parameters[1].gpr();
        RELEASE_ASSERT(result != X86Registers::ecx);

        if constexpr (opcode == Shl) {
            if constexpr (width == Width32)
                jit.lshift32(value, X86Registers::ecx, result);
            else
                jit.lshift64(value, X86Registers::ecx, result);
        } else if constexpr (opcode == SShr) {
            if constexpr (width == Width32)
                jit.rshift32(value, X86Registers::ecx, result);
            else
                jit.rshift64(value, X86Registers::ecx, result);
        } else {
            if constexpr (width == Width32)
                jit.urshift32(value, X86Registers::ecx, result);
            else
                jit.urshift64(value, X86Registers::ecx, result);
        }
    }
};

static PatchOperation* beginFixedRegisterPatch(Builder& builder, Opcode opcode, Type type)
{
    PatchOperation* patch = builder.patch(type);
    patch->effects = Effects::none();
    patch->setRunsWithoutFrame(true);
    patch->pureFunction = PureFunction { PureFunction::firstIDOfBackEnd | opcode, true };
    return patch;
}

template<typename Generator32, typename Generator64>
static PatchOperation* finishFixedRegisterPatch(Builder& builder, PatchOperation* patch)
{
    patch->setClearsUpperHalf(patch->type() == Int32);
    if (patch->type() == Int32)
        patch->setGenerator(Generator32());
    else
        patch->setGenerator(Generator64());
    builder.recordPurePatch(patch);
    return patch;
}

template<Opcode opcode>
static PatchOperation* appendFixedRegisterDivision(Builder& builder, Type type, Operation* dividend, Operation* divisor)
{
    ASSERT(dividend->type() == type && divisor->type() == type);
    constexpr bool yieldsQuotient = opcode == Div || opcode == UDiv;
    constexpr GPRReg result = yieldsQuotient ? X86Registers::eax : X86Registers::edx;
    constexpr GPRReg byproduct = yieldsQuotient ? X86Registers::edx : X86Registers::eax;

    PatchOperation* patch = beginFixedRegisterPatch(builder, opcode, type);
    patch->append(dividend, ValueRep::reg(X86Registers::eax));
    patch->append(divisor, ValueRep::SomeLateRegister);
    patch->resultConstraints = { ValueRep::reg(result) };
    patch->clobberEarly(maskOf(Reg(X86Registers::edx)));
    patch->clobberLate(maskOf(Reg(byproduct)));
    return finishFixedRegisterPatch<FixedRegisterDivisionGenerator<opcode, Width32>, FixedRegisterDivisionGenerator<opcode, Width64>>(builder, patch);
}

template<Opcode opcode>
static PatchOperation* appendFixedRegisterMultiplication(Builder& builder, Type type, Operation* left, Operation* right)
{
    ASSERT(left->type() == type && right->type() == type);

    PatchOperation* patch = beginFixedRegisterPatch(builder, opcode, type);
    patch->append(left, ValueRep::SomeRegister);
    patch->append(right, ValueRep::reg(X86Registers::eax));
    patch->resultConstraints = { ValueRep::reg(X86Registers::edx) };
    patch->clobberLate(maskOf(Reg(X86Registers::eax)));
    return finishFixedRegisterPatch<FixedRegisterMultiplicationGenerator<opcode, Width32>, FixedRegisterMultiplicationGenerator<opcode, Width64>>(builder, patch);
}

template<Opcode opcode>
static PatchOperation* appendFixedRegisterShift(Builder& builder, Type type, Operation* value, Operation* amount)
{
    ASSERT(value->type() == type && amount->type() == Int32);

    PatchOperation* patch = beginFixedRegisterPatch(builder, opcode, type);
    patch->append(value, ValueRep::SomeRegister);
    patch->append(amount, ValueRep::reg(X86Registers::ecx));
    // The generator moves the value into the result register before it reads rcx, so the result must not be rcx. The late clobber keeps it out.
    patch->clobberLate(maskOf(Reg(X86Registers::ecx)));
    return finishFixedRegisterPatch<FixedRegisterShiftGenerator<opcode, Width32>, FixedRegisterShiftGenerator<opcode, Width64>>(builder, patch);
}

Operation* appendFoldedProduct(Builder& builder, Operation* left, Operation* right)
{
    RELEASE_ASSERT(left->type() == Int64 && right->type() == Int64);
    constexpr PureFunction function { PureFunction::firstIDOfBackEnd | numberOfOpcodes, true };
    Operation* operands[] = { left, right };
    if (Operation* existing = builder.findPurePatch(function, Int32, operands))
        return existing;

    PatchOperation* patch = builder.patch(Int32);
    patch->effects = Effects::none();
    patch->setRunsWithoutFrame(true);
    patch->pureFunction = function;
    patch->append(left, ValueRep::SomeRegister);
    patch->append(right, ValueRep::reg(X86Registers::eax));
    patch->resultConstraints = { ValueRep::reg(X86Registers::eax) };
    patch->clobberLate(maskOf(Reg(X86Registers::edx)));
    patch->setClearsUpperHalf(true);
    patch->setGenerator(FoldedProductGenerator());
    builder.recordPurePatch(patch);
    return patch;
}

PatchOperation* appendWithFixedRegisters(Builder& builder, Opcode opcode, Type type, Operation* left, Operation* right)
{
    RELEASE_ASSERT(type.isInt());

    switch (opcode) {
    case MulHigh:
        return appendFixedRegisterMultiplication<MulHigh>(builder, type, left, right);
    case UMulHigh:
        return appendFixedRegisterMultiplication<UMulHigh>(builder, type, left, right);
    case Div:
        return appendFixedRegisterDivision<Div>(builder, type, left, right);
    case UDiv:
        return appendFixedRegisterDivision<UDiv>(builder, type, left, right);
    case Mod:
        return appendFixedRegisterDivision<Mod>(builder, type, left, right);
    case UMod:
        return appendFixedRegisterDivision<UMod>(builder, type, left, right);
    case Shl:
        return appendFixedRegisterShift<Shl>(builder, type, left, right);
    case SShr:
        return appendFixedRegisterShift<SShr>(builder, type, left, right);
    case ZShr:
        return appendFixedRegisterShift<ZShr>(builder, type, left, right);
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT) && CPU(X86_64)
