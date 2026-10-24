/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMathBuiltinEmitter.h"

#if ENABLE(AOT)

#include "AOTElementAccessEmitter.h"
#include "AOTOperatorEmitter.h"
#include "AOTRepresentationRules.h"
#include "AOTTypeTable.h"
#include "AOTVector.h"
#include "MathCommon.h"
#include <algorithm>
#include <limits>
#include <wtf/WeakRandom.h>

namespace JSC { namespace AOT { namespace Typed {

static bool areIntegers(const Function& function, const InlineCall& call)
{
    return std::ranges::all_of(call.arguments(), [&](ValueID argument) {
        return integerRepresentations.contains(function.at(argument).representation);
    });
}

static double extremumOfNoValues(bool isMax)
{
    return isMax ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
}

static RuntimeOperation runtimeOperationOf(MathFunctionKind kind)
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

Number MathBuiltinEmitter::extremumOfElements(Extremum kind, CellPointer array, Pointer butterfly, Integer length, LBasicBlock otherwise)
{
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    ArrayElements storage { array, butterfly };
    bool isMax = kind == Extremum::Max;
    Join<Number> join(m_out);
    auto fold = [&](const ArrayStorageForm& form) {
        bool foldPropagatesHoles = form.element == Representation::Double;
        LBasicBlock loop = newBlock();
        LBasicBlock element = newBlock();
        LBasicBlock done = newBlock();
        ValueFromBlock firstIndex = m_out.anchor(integer(0).value());
        ValueFromBlock initialValue = m_out.anchor(number(extremumOfNoValues(isMax)).value());
        m_out.jump(loop);
        m_out.appendTo(loop);
        Integer index(m_out.phi(B3::Int64, firstIndex));
        Number chosen(m_out.phi(B3::Double, initialValue));
        branch(below(index, length), { normalEdge(element), normalEdge(done) });
        m_out.appendTo(element);
        Operand raw = elements.loadRawElement(form, storage, index);
        if (form.element == Representation::JSValue)
            branchUnless(isNumber(raw.as<Boxed>()), otherwise);
        else if (!foldPropagatesHoles)
            branchUnless(logicalNot(elements.isHole(form, raw)), otherwise);
        Number next = elements.convertRawElement(form, raw, Representation::Double, m_program.at(TypeTable::number)).as<Number>();
        m_out.addIncomingToPhi(index.value(), m_out.anchor(add(index, integer(1)).value()));
        m_out.addIncomingToPhi(chosen.value(), m_out.anchor((isMax ? max(chosen, next) : min(chosen, next)).value()));
        m_out.jump(loop);
        m_out.appendTo(done);
        if (foldPropagatesHoles)
            branchUnless(logicalNot(elements.isHole(form, chosen)), otherwise);
        join.add(chosen);
    };
    const ArrayStorageForm& doubleForm = arrayStorageFormOf(ArrayStorage::Double);
    const ArrayStorageForm& int32Form = arrayStorageFormOf(ArrayStorage::Int32);
    const ArrayStorageForm& boxedForm = arrayStorageFormOf(ArrayStorage::Contiguous);
    Word32 mode = elements.indexingMode(array, arrayStorageTestMask);
    LBasicBlock doubles = newBlock();
    LBasicBlock notDoubles = newBlock();
    LBasicBlock int32s = newBlock();
    LBasicBlock notInt32s = newBlock();
    branch(elements.hasShape(mode, doubleForm), { normalEdge(doubles), normalEdge(notDoubles) });
    m_out.appendTo(doubles);
    fold(doubleForm);
    m_out.appendTo(notDoubles);
    branch(elements.hasShape(mode, int32Form), { normalEdge(int32s), normalEdge(notInt32s) });
    m_out.appendTo(int32s);
    fold(int32Form);
    m_out.appendTo(notInt32s);
    branchUnless(elements.hasShape(mode, boxedForm), otherwise);
    fold(boxedForm);
    return join.finish();
}

void MathBuiltinEmitter::extremumOfArray(const InlineCall& call, Extremum kind)
{
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    ArrayElements storage = elements.elementsOf(cellOperand(call.argument(0)));
    finish(call, extremumOfElements(kind, storage.array, storage.butterfly, elements.publicLength(storage), missBlockOf(call)));
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
        finish(call, select(greaterThan(sub(ceiling, number(0.5)), x), sub(ceiling, number(1)), ceiling));
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
    if (!areIntegers(m_function, call)) {
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
    if (!areIntegers(m_function, call)) {
        Number x = numberOperand(argument);
        finish(call, select(greaterThan(x, number(0)), number(1), select(lessThan(x, number(0)), number(-1), x)));
        return;
    }
    Integer x = integerOperand(argument);
    Integer isNegative(m_out.aShr(x.value(), m_out.constInt32(63)));
    Integer isPositive(m_out.lShr(neg(x).value(), m_out.constInt32(63)));
    finish(call, bitOr(isNegative, isPositive), Range { -1, 1 });
}

void MathBuiltinEmitter::generateMathFunction(const InlineCall& call)
{
    finish(call, callLeaf<Number>(runtimeOperationOf(call.parameter<MathFunctionKind>(0)), { numberOperand(call.argument(0)) }));
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
    const B3::AbstractHeap& runtimeState = m_heaps.entire(AbstractHeap::RuntimeState);
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
    LValue bits = m_out.bitAnd(m_out.add(x, y), m_out.constInt64((1ULL << 53) - 1));
    finish(call, mul(Number(m_out.intToDouble(bits)), number(1.0 / (1ULL << 53))));
}

void MathBuiltinEmitter::generateMathPow(const InlineCall& call)
{
    Number base = numberOperand(call.argument(0));
    Number exponent = numberOperand(call.argument(1));
    finish(call, emitter<OperatorEmitter>().pow(base, exponent));
}

void MathBuiltinEmitter::extremum(const InlineCall& call, Extremum kind)
{
    std::span<const ValueID> arguments = call.arguments();
    bool isMax = kind == Extremum::Max;
    if (arguments.empty()) {
        finish(call, number(extremumOfNoValues(isMax)));
        return;
    }
    if (areIntegers(m_function, call)) {
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

void MathBuiltinEmitter::generateMathMinOfArray(const InlineCall& call)
{
    extremumOfArray(call, Extremum::Min);
}

void MathBuiltinEmitter::generateMathMaxOfArray(const InlineCall& call)
{
    extremumOfArray(call, Extremum::Max);
}

void MathBuiltinEmitter::generateMathImul(const InlineCall& call)
{
    Word32 x = int32Operand(call.argument(0));
    Word32 y = int32Operand(call.argument(1));
    finish(call, mul(x, y), m_program.at(TypeTable::int32));
}

void MathBuiltinEmitter::generateMathClz32(const InlineCall& call)
{
    finish(call, countLeadingZeros(int32Operand(call.argument(0))), Type::integer({ 0, 32 }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
