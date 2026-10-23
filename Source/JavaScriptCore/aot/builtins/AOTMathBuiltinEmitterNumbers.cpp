/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMathBuiltinEmitter.h"

#if ENABLE(AOT)

#include "AOTStringEmitter.h"
#include "AOTStringRangeReadEmitter.h"
#include "AOTTypeTable.h"
#include "MathCommon.h"
#include <limits>

namespace JSC { namespace AOT { namespace Typed {

Condition MathBuiltinEmitter::isFinite(Number x)
{
    return equal(sub(x, x), number(0));
}

Condition MathBuiltinEmitter::testNumber(Number x, NumberTest test)
{
    switch (test) {
    case NumberTest::IsNaN:
        return notEqualOrUnordered(x, x);
    case NumberTest::IsFinite:
        return isFinite(x);
    case NumberTest::IsInteger:
        return equal(sub(x, trunc(x)), number(0));
    case NumberTest::IsSafeInteger:
        return logicalAnd(equal(trunc(x), x), lessThanOrEqual(abs(x), number(maxSafeInteger())));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void MathBuiltinEmitter::testNumber(const InlineCall& call, NumberTest test)
{
    ValueID argument = call.argument(0);
    if (typeOf(argument).isNumber()) {
        finish(call, testNumber(numberOperand(argument), test));
        return;
    }
    Boxed value = boxedOperand(call.context(), argument);
    finish(call, either<Condition>(isInt32(value), [&] {
        return boolean(test != NumberTest::IsNaN);
    }, [&] {
        return either<Condition>(isNumber(value), [&] {
            return testNumber(unboxDouble(value), test);
        }, [&] {
            return boolean(false);
        });
    }));
}

void MathBuiltinEmitter::generateIsNaN(const InlineCall& call)
{
    testNumber(call, NumberTest::IsNaN);
}

void MathBuiltinEmitter::generateIsFinite(const InlineCall& call)
{
    testNumber(call, NumberTest::IsFinite);
}

void MathBuiltinEmitter::generateNumberIsNaN(const InlineCall& call)
{
    testNumber(call, NumberTest::IsNaN);
}

void MathBuiltinEmitter::generateNumberIsFinite(const InlineCall& call)
{
    testNumber(call, NumberTest::IsFinite);
}

void MathBuiltinEmitter::generateNumberIsInteger(const InlineCall& call)
{
    testNumber(call, NumberTest::IsInteger);
}

void MathBuiltinEmitter::generateNumberIsSafeInteger(const InlineCall& call)
{
    testNumber(call, NumberTest::IsSafeInteger);
}

Number MathBuiltinEmitter::stringToNumber(const InlineCall& call, CellPointer string)
{
    return emitter<StringRangeReadEmitter>().toNumber(emitter<StringEmitter>().viewOfString(call.context(), string));
}

void MathBuiltinEmitter::generateCallNumberConstructor(const InlineCall& call)
{
    ValueID argument = call.argument(0);
    const Type& type = typeOf(argument);
    if (type.isAnyString()) {
        finish(call, stringToNumber(call, cellOperand(argument)));
        return;
    }
    Boxed operand = boxedOperand(call.context(), argument);
    if (type.isNumber()) {
        finish(call, operand, m_program.at(TypeTable::number));
        return;
    }
    auto is = [&](int64_t bits) {
        return Condition(m_out.equal(operand.value(), m_out.constInt64(bits)));
    };
    finish(call, either<Number>(isNotCell(operand), [&] {
        return either<Number>(isNumber(operand), [&] {
            return numberToDouble(operand);
        }, [&] {
            return select(is(JSValue::ValueUndefined), number(PNaN), select(is(JSValue::ValueTrue), number(1), number(0)));
        });
    }, [&] {
        CellPointer cell = unboxCell(operand);
        missUnless(call, hasCellType(cell, StringType));
        return stringToNumber(call, cell);
    }));
}

void MathBuiltinEmitter::generateParseInt(const InlineCall& call)
{
    std::optional<Word32> radix;
    if (ValueID argument = call.argumentOrNull(1))
        radix = int32Operand(argument);
    CellPointer string = cellOperand(call.argument(0));
    missUnless(call, logicalNot(isRopeString(string)));
    CharacterView view = emitter<StringEmitter>().viewOfFlatString(string);
    finish(call, emitter<StringRangeReadEmitter>().parseInt(view, radix.value_or(word32(0))));
}

void MathBuiltinEmitter::generateNumberToString(const InlineCall& call)
{
    ValueID receiver = call.receiver();
    bool hasInt32Receiver = representationOf(receiver) == Representation::Int32;
    if (!call.numberOfArguments()) {
        StringEmitter& strings = emitter<StringEmitter>();
        finish(call, hasInt32Receiver ? strings.integerToString(call.context(), int32Operand(receiver)) : strings.numberToString(call.context(), numberOperand(receiver)));
        return;
    }
    ValueID argument = call.argument(0);
    Integer wideRadix = integerOperand(argument);
    if (!typeOf(argument).integers.isSubsetOf({ 2, 36 }))
        missUnless(call, below(sub(wideRadix, integer(2)), integer(35)));
    Word32 radix = low32(wideRadix);
    if (hasInt32Receiver) {
        finish(call, callOperation<CellPointer>(call.context(), RuntimeOperation::Int32ToStringWithRadix, { int32Operand(receiver), radix }));
        return;
    }
    finish(call, callOperation<CellPointer>(call.context(), RuntimeOperation::NumberToStringWithRadix, { numberOperand(receiver), radix }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
