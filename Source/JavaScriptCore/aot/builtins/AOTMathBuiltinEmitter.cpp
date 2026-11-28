/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMathBuiltinEmitter.h"

#if ENABLE(AOT)

#include "AOTConversionEmitter.h"
#include "AOTElementAccessEmitter.h"
#include "AOTIRProgram.h"
#include "AOTOperatorEmitter.h"
#include "AOTRepresentationRules.h"
#include "AOTTypeArena.h"
#include "AOTTypeTable.h"
#include "AOTVector.h"
#include "MathCommon.h"
#include <algorithm>
#include <limits>
#include <wtf/WeakRandom.h>

namespace JSC { namespace AOT { namespace Typed {

static bool mathArgumentsAreIntegers(const Code& code, const InlineCall& call)
{
    return std::ranges::all_of(call.arguments(), [&](ValueID argument) {
        return integerRepresentations.contains(code.at(argument).representation);
    });
}

static double extremumOfNoValues(bool isMax)
{
    return isMax ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
}

static RuntimeOperation runtimeOperationOfMathFunction(MathFunctionKind kind)
{
    switch (kind) {
#define AOT_MATH_FUNCTION_OPERATION(capitalizedName, lowerName) \
    case MathFunctionKind::capitalizedName: \
        return RuntimeOperation::Math ## capitalizedName;
    FOR_EACH_ARITH_UNARY_OP(AOT_MATH_FUNCTION_OPERATION)
#undef AOT_MATH_FUNCTION_OPERATION
    }
    RELEASE_ASSERT_NOT_REACHED();
}

MathBuiltinEmitter::MathBuiltinEmitter(CodeGenerator& generator)
    : BuiltinFamilyEmitter(generator)
{
}

void MathBuiltinEmitter::round(const InlineCall& call, Rounding rounding)
{
    Number x = numberOperand(call.argument(0));
    switch (rounding) {
    case Rounding::Floor:
        finish(call, floor(x));
        return;
    case Rounding::Ceil:
        finish(call, ceil(x));
        return;
    case Rounding::Trunc:
        finish(call, trunc(x));
        return;
    case Rounding::Round: {
        Number ceiling = ceil(x);
        Condition isNearerToFloor = greaterThan(sub(ceiling, 0.5), x);
        finish(call, select(isNearerToFloor, sub(ceiling, 1), ceiling));
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void MathBuiltinEmitter::generateMathFloor(const InlineCall& call)
{
    round(call, Rounding::Floor);
}

void MathBuiltinEmitter::generateMathCeil(const InlineCall& call)
{
    round(call, Rounding::Ceil);
}

void MathBuiltinEmitter::generateMathTrunc(const InlineCall& call)
{
    round(call, Rounding::Trunc);
}

void MathBuiltinEmitter::generateMathRound(const InlineCall& call)
{
    round(call, Rounding::Round);
}

void MathBuiltinEmitter::generateMathAbs(const InlineCall& call)
{
    ValueID argument = call.argument(0);
    if (!mathArgumentsAreIntegers(m_code, call)) {
        finish(call, abs(numberOperand(argument)));
        return;
    }
    Integer x = integerOperand(argument);
    Integer sign(m_out.aShr(x.value(), m_out.constInt32(63)));
    Integer magnitude = sub(bitXor(x, sign), sign);
    Range range = typeOf(argument).integers;
    finish(call, magnitude, Range { 0, std::max(-range.minimum, range.maximum) });
}

void MathBuiltinEmitter::generateMathFround(const InlineCall& call)
{
    Number x = numberOperand(call.argument(0));
    finish(call, Number(m_out.fround(x.value())));
}

void MathBuiltinEmitter::generateMathF16Round(const InlineCall& call)
{
    finish(call, callLeaf<Number>(RuntimeOperation::MathF16Round, { numberOperand(call.argument(0)) }));
}

void MathBuiltinEmitter::generateMathSqrt(const InlineCall& call)
{
    finish(call, sqrt(numberOperand(call.argument(0))));
}

void MathBuiltinEmitter::generateMathSign(const InlineCall& call)
{
    ValueID argument = call.argument(0);
    if (!mathArgumentsAreIntegers(m_code, call)) {
        Number x = numberOperand(argument);
        finish(call, selectByBranch<Number>(greaterThan(x, number(0)), [&] {
            return number(1);
        }, [&] {
            return selectByBranch<Number>(lessThan(x, number(0)), [&] {
                return number(-1);
            }, [&] {
                return emitter<ConversionEmitter>().purified(x);
            });
        }));
        return;
    }
    Integer x = integerOperand(argument);
    Integer isNegative(m_out.aShr(x.value(), m_out.constInt32(63)));
    Integer isPositive(m_out.lShr(neg(x).value(), 63));
    finish(call, bitOr(isNegative, isPositive), Range { -1, 1 });
}

void MathBuiltinEmitter::generateMathFunction(const InlineCall& call)
{
    finish(call, callLeaf<Number>(runtimeOperationOfMathFunction(call.parameter<MathFunctionKind>(0)), { numberOperand(call.argument(0)) }));
}

void MathBuiltinEmitter::generateMathATan2(const InlineCall& call)
{
    Number y = numberOperand(call.argument(0));
    Number x = numberOperand(call.argument(1));
    finish(call, callLeaf<Number>(RuntimeOperation::MathATan2, { y, x }));
}

void MathBuiltinEmitter::generateMathHypot(const InlineCall& call)
{
    Vector<Number, 3> numbers;
    for (ValueID argument : call.arguments())
        numbers.append(numberOperand(argument));
    switch (numbers.size()) {
    case 0:
        finish(call, number(0));
        return;
    case 1:
        finish(call, abs(numbers[0]));
        return;
    case 2:
        finish(call, callLeaf<Number>(RuntimeOperation::MathHypot, { numbers[0], numbers[1] }));
        return;
    case 3:
        finish(call, callLeaf<Number>(RuntimeOperation::MathHypot3, { numbers[0], numbers[1], numbers[2] }));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void MathBuiltinEmitter::generateMathRandom(const InlineCall& call)
{
    const Machine::AbstractHeap& runtimeState = m_heaps.entire(AbstractHeap::RuntimeState);
    LValue globalObject = loadImmutablePointer(AOT_INSTANCE_OFFSET(globalObject)).value();
    TypedPointer lowAddress = m_out.address(runtimeState, globalObject, JSGlobalObject::weakRandomOffset() + WeakRandom::lowOffset());
    TypedPointer highAddress = m_out.address(runtimeState, globalObject, JSGlobalObject::weakRandomOffset() + WeakRandom::highOffset());
    LValue x = m_out.load64(lowAddress);
    LValue y = m_out.load64(highAddress);
    m_out.store64(y, lowAddress);
    x = m_out.bitXor(x, m_out.shl(x, m_out.constInt32(23)));
    x = m_out.bitXor(x, m_out.lShr(x, m_out.constInt32(17)));
    x = m_out.bitXor(x, m_out.bitXor(y, m_out.lShr(y, m_out.constInt32(26))));
    m_out.store64(x, highAddress);
    LValue bits = m_out.bitAnd(m_out.add(x, y), (1ULL << 53) - 1);
    finish(call, mul(Number(m_out.intToDouble(bits)), 1.0 / (1ULL << 53)));
}

void MathBuiltinEmitter::generateMathPow(const InlineCall& call)
{
    Number base = numberOperand(call.argument(0));
    Number exponent = numberOperand(call.argument(1));
    BlockID block = call.context().block();
    finish(call, emitter<OperatorEmitter>().pow(base, exponent, block && generator().usesLoopForms(block)));
}

void MathBuiltinEmitter::extremum(const InlineCall& call, Extremum kind)
{
    std::span<const ValueID> arguments = call.arguments();
    bool isMax = kind == Extremum::Max;
    if (arguments.empty()) {
        finish(call, number(extremumOfNoValues(isMax)));
        return;
    }
    if (mathArgumentsAreIntegers(m_code, call) && std::ranges::all_of(arguments, [&](ValueID argument) { return typeOf(argument).isInt32(); })) {
        Word32 chosen = int32Operand(arguments[0]);
        Range range = typeOf(arguments[0]).integers;
        for (ValueID argument : arguments.subspan(1)) {
            Word32 next = int32Operand(argument);
            chosen = select(isMax ? greaterThan(chosen, next) : lessThan(chosen, next), chosen, next);
            range = range.unionWith(typeOf(argument).integers);
        }
        finish(call, chosen, types().at(types().integer(range)));
        return;
    }
    if (mathArgumentsAreIntegers(m_code, call)) {
        Integer chosen = integerOperand(arguments[0]);
        Range range = typeOf(arguments[0]).integers;
        for (ValueID argument : arguments.subspan(1)) {
            Integer next = integerOperand(argument);
            chosen = select(isMax ? greaterThan(chosen, next) : lessThan(chosen, next), chosen, next);
            range = range.unionWith(typeOf(argument).integers);
        }
        finish(call, chosen, range);
        return;
    }
    Number chosen = numberOperand(arguments[0]);
    for (ValueID argument : arguments.subspan(1)) {
        Number next = numberOperand(argument);
        chosen = isMax ? max(chosen, next) : min(chosen, next);
    }
    finish(call, chosen);
}

void MathBuiltinEmitter::generateMathMin(const InlineCall& call)
{
    extremum(call, Extremum::Min);
}

void MathBuiltinEmitter::generateMathMax(const InlineCall& call)
{
    extremum(call, Extremum::Max);
}

void MathBuiltinEmitter::generateMathImul(const InlineCall& call)
{
    Word32 x = int32Operand(call.argument(0));
    Word32 y = int32Operand(call.argument(1));
    finish(call, mul(x, y), m_program.at(TypeTable::int32));
}

void MathBuiltinEmitter::generateMathClz32(const InlineCall& call)
{
    finish(call, countLeadingZeros(int32Operand(call.argument(0))), types().at(types().integer({ 0, 32 })));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
