/*
 *  Copyright (C) 1999-2000 Harri Porten (porten@kde.org)
 *  Copyright (C) 2002-2020 Apple Inc. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public License
 *  along with this library; see the file COPYING.LIB.  If not, write to
 *  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301, USA.
 *
 */

#pragma once

#include "JSExportMacros.h"
#include <wtf/Forward.h>
#include <wtf/TriState.h>

namespace JSC {

class JSBigInt;
class JSCell;
class JSGlobalObject;
class JSString;
class JSValue;
class Register;
class VM;

enum class Concurrency : uint8_t;
enum class OverloadableOperator : uint8_t;
enum class JSBigIntComparisonMode : uint8_t;
enum class JSBigIntComparisonResult : uint8_t;

typedef int64_t EncodedJSValue;

#define InvalidPrototypeChain (std::numeric_limits<size_t>::max())

NEVER_INLINE JSValue jsAddSlowCase(JSGlobalObject*, JSValue, JSValue);

// For when either operand overloadsOperators(). The right operand of an operator that has none is empty.
JS_EXPORT_PRIVATE NEVER_INLINE JSValue callOverloadedOperator(JSGlobalObject*, OverloadableOperator, JSValue left, JSValue right);
JS_EXPORT_PRIVATE NEVER_INLINE bool compareWithOverloadedOperator(JSGlobalObject*, OverloadableOperator, JSValue left, JSValue right);

// ---- String concatenation and overloaded operators
//
// A string plus anything is a string, so a + b + c where a or b is known to be a string is not compiled to two additions. Each operand is converted to a primitive
// where the addition that it is an operand of would have done it, by op_to_primitive, and one op_strcat concatenates them all. See BinaryOpNode::emitStrcat().
//
// Except that a string plus an object that overloads operators is whatever the object says. That is found out where little else pays for it: op_to_primitive
// leaves the interpreter for any object. If the object overloads operators the additions so far are carried out there and then, which is when they would have been. What
// they come to is left in place of the operand, in a cell that says that it stands for that operand and all those to the left of it. It need not be a string, so
// from then on every operand is added to it as it is come to: op_to_primitive also leaves the interpreter if what the operand before it left is an object. A string literal
// does not go through op_to_primitive. It is put in its register before the operand to the left of it does, there being nothing to tell the difference, and is added along
// with that. An op_strcat that comes across an object has only to take out what it all came to.
//
// In d += a + b + c, d is converted last and added to what the rest come to.
//
// Operands are given as the address of the first and the distance to the next, in Registers: -1 in a call frame, 1 in a buffer.
struct OperandsOfStringConcatenation {
    const Register* first;
    int stride;
    JSValue at(unsigned index) const;
};
// For operand `index`. The operand after the first has been evaluated by the time the first is converted.
JS_EXPORT_PRIVATE JSValue toPrimitiveForStringConcatenation(JSGlobalObject*, OperandsOfStringConcatenation, unsigned index, unsigned previous, unsigned literalsAfter);
// For d in d += a + b + c. `previous` is what the last of the others to be converted left.
JS_EXPORT_PRIVATE JSValue toPrimitiveForTargetOfStringConcatenation(JSGlobalObject*, JSValue target, JSValue previous);
// For when one of them is an object.
JS_EXPORT_PRIVATE JSValue addUpInsteadOfConcatenating(JSGlobalObject*, OperandsOfStringConcatenation, unsigned count, unsigned firstOperand);

// operand + "" and "" + operand are not compiled to an addition either, but to an op_to_primitive, which is told which it is with these, and an op_to_string. It goes
// the same way: what an object that overloads operators says is left in such a cell, and the op_to_string takes it out.
enum AdditionOfEmptyString : unsigned {
    NoAdditionOfEmptyString = 0,
    EmptyStringIsOnTheLeft = 1,
    EmptyStringIsOnTheRight = 2,
    AdditionOfEmptyStringIsCompoundAssignment = 4,
};
JS_EXPORT_PRIVATE JSValue toPrimitiveForAdditionOfEmptyString(JSGlobalObject*, JSValue, unsigned addition);
JS_EXPORT_PRIVATE JSValue toStringForAdditionOfEmptyString(JSGlobalObject*, JSValue);
JSString* jsTypeStringForValueWithConcurrency(VM&, JSGlobalObject*, JSValue, Concurrency);
size_t normalizePrototypeChain(JSGlobalObject*, JSCell*, bool& sawPolyProto);

// All inline function definitions are in OperationsInlines.h
template<Concurrency concurrency>
TriState jsTypeofIsObjectWithConcurrency(JSGlobalObject*, JSValue);
template<Concurrency concurrency>
TriState jsTypeofIsFunctionWithConcurrency(JSGlobalObject*, JSValue);
JSString* jsTypeStringForValue(JSGlobalObject*, JSValue);
bool jsTypeofIsObject(JSGlobalObject*, JSValue);
bool jsTypeofIsFunction(JSGlobalObject*, JSValue);

JSString* jsString(JSGlobalObject*, const String&, JSString*);
JSString* jsString(JSGlobalObject*, JSString*, const String&);
JSString* jsString(JSGlobalObject*, JSString*, JSString*);
JSString* jsString(JSGlobalObject*, JSString*, JSString*, JSString*);
JSString* jsString(JSGlobalObject*, const String&, const String&);
JSString* jsString(JSGlobalObject*, const String&, const String&, const String&);
JSValue jsStringFromRegisterArray(JSGlobalObject*, Register*, unsigned count, unsigned firstOperand);

JSBigIntComparisonResult compareBigInt(JSValue, JSValue);
JSBigIntComparisonResult compareBigIntToOtherPrimitive(JSGlobalObject*, JSBigInt*, JSValue);
JSBigIntComparisonResult compareBigInt32ToOtherPrimitive(JSGlobalObject*, int32_t, JSValue);
bool bigIntCompareResult(JSBigIntComparisonResult, JSBigIntComparisonMode);
bool bigIntCompare(JSGlobalObject*, JSValue, JSValue, JSBigIntComparisonMode);
bool toPrimitiveNumeric(JSGlobalObject*, JSValue, JSValue&, double&);

template<bool leftFirst>
bool jsLess(JSGlobalObject*, JSValue, JSValue);
template<bool leftFirst>
bool jsLessEq(JSGlobalObject*, JSValue, JSValue);

JSValue jsAddNonNumber(JSGlobalObject*, JSValue, JSValue);
JSValue jsAdd(JSGlobalObject*, JSValue, JSValue);

template<OverloadableOperator, typename DoubleOperation, typename BigIntOp>
JSValue arithmeticBinaryOp(JSGlobalObject*, JSValue, JSValue, DoubleOperation&&, BigIntOp&&, ASCIILiteral);

JSValue jsSub(JSGlobalObject*, JSValue, JSValue);
JSValue jsMul(JSGlobalObject*, JSValue, JSValue);
JSValue jsDiv(JSGlobalObject*, JSValue, JSValue);
JSValue jsRemainder(JSGlobalObject*, JSValue, JSValue);
JSValue jsPow(JSGlobalObject*, JSValue, JSValue);
JSValue jsNegate(JSGlobalObject*, JSValue);
JSValue jsToNumericForPostfix(JSGlobalObject*, JSValue);
JSValue jsInc(JSGlobalObject*, JSValue);
JSValue jsDec(JSGlobalObject*, JSValue);
JSValue jsBitwiseNot(JSGlobalObject*, JSValue);

template <bool isLeft>
JSValue shift(JSGlobalObject*, JSValue, JSValue);
JSValue jsLShift(JSGlobalObject*, JSValue, JSValue);
JSValue jsRShift(JSGlobalObject*, JSValue, JSValue);
JSValue jsURShift(JSGlobalObject*, JSValue, JSValue);

template<OverloadableOperator, typename Int32Operation, typename BigIntOp>
JSValue bitwiseBinaryOp(JSGlobalObject*, JSValue, JSValue, Int32Operation&&, BigIntOp&&, ASCIILiteral);
JSValue jsBitwiseAnd(JSGlobalObject*, JSValue, JSValue);
JSValue jsBitwiseOr(JSGlobalObject*, JSValue, JSValue);
JSValue jsBitwiseXor(JSGlobalObject*, JSValue, JSValue);

EncodedJSValue getByValWithIndexAndThis(JSGlobalObject*, JSCell*, uint32_t, JSValue);
EncodedJSValue getByValWithIndex(JSGlobalObject*, JSCell*, uint32_t);

} // namespace JSC
