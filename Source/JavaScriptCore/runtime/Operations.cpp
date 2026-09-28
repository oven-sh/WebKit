/*
 * Copyright (C) 1999-2000 Harri Porten (porten@kde.org)
 * Copyright (C) 2008-2020 Apple Inc. All rights reserved.
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

#include "config.h"
#include "Operations.h"

#include "BytecodeStructs.h"
#include "CodeBlock.h"
#include "JSBigInt.h"
#include "JSCInlines.h"

namespace JSC {

bool JSValue::equalSlowCase(JSGlobalObject* globalObject, JSValue v1, JSValue v2)
{
    return equalSlowCaseInline(globalObject, v1, v2);
}

// Whether the operator that is being carried out is that of x += y and its like. Nothing that is compiled for an operator says, since it makes no
// difference to any of it. The instruction does, and it can be found from here as it is for saying what was being evaluated when something went wrong.
static bool isCarryingOutCompoundAssignment(VM& vm)
{
    CallFrame* callFrame = vm.topCallFrame;
    if (!callFrame || callFrame->isNativeCalleeFrame())
        return false;
    CodeBlock* codeBlock = callFrame->codeBlock();
    if (!codeBlock)
        return false;
    CodeOrigin origin = callFrame->codeOrigin();
    if (!origin.isSet())
        return false;
    CodeBlock* baseline = baselineCodeBlockForOriginAndBaselineCodeBlock(origin, codeBlock->baselineAlternative());
    auto instruction = baseline->instructions().at(origin.bytecodeIndex());
    switch (instruction->opcodeID()) {
#define CASE(Op) \
    case Op::opcodeID: \
        return instruction->as<Op>().m_operandTypes.isCompoundAssignment();
    CASE(OpAdd)
    CASE(OpSub)
    CASE(OpMul)
    CASE(OpDiv)
    CASE(OpMod)
    CASE(OpPow)
    CASE(OpLshift)
    CASE(OpRshift)
    CASE(OpBitand)
    CASE(OpBitor)
    CASE(OpBitxor)
#undef CASE
    default:
        return false;
    }
}

JSValue callOverloadedOperator(JSGlobalObject* globalObject, OverloadableOperator op, JSValue left, JSValue right)
{
    ASSERT(left.overloadsOperators() || right.overloadsOperators());
    JSCell* cell = left.overloadsOperators() ? left.asCell() : right.asCell();
    bool isCompoundAssignment = op <= OverloadableOperator::BitwiseXor && isCarryingOutCompoundAssignment(globalObject->vm());
    return cell->methodTable()->operate(globalObject, op, left, right, isCompoundAssignment);
}

bool compareWithOverloadedOperator(JSGlobalObject* globalObject, OverloadableOperator op, JSValue left, JSValue right)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT(op >= OverloadableOperator::Equal);
    JSValue result = callOverloadedOperator(globalObject, op, left, right);
    RETURN_IF_EXCEPTION(scope, false);
    ASSERT(result.isBoolean());
    return result.asBoolean();
}

static JSValue addWithoutOverloading(JSGlobalObject*, JSValue, JSValue);

NEVER_INLINE JSValue jsAddSlowCase(JSGlobalObject* globalObject, JSValue v1, JSValue v2)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (v1.overloadsOperators() || v2.overloadsOperators()) [[unlikely]] {
        JSValue result = callOverloadedOperator(globalObject, OverloadableOperator::Add, v1, v2);
        RETURN_IF_EXCEPTION(scope, { });
        if (result)
            return result;
        ASSERT(v1.isString() || v2.isString());
    }
    RELEASE_AND_RETURN(scope, addWithoutOverloading(globalObject, v1, v2));
}

static JSValue addWithoutOverloading(JSGlobalObject* globalObject, JSValue v1, JSValue v2)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSValue p1 = v1.toPrimitive(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue p2 = v2.toPrimitive(globalObject);
    RETURN_IF_EXCEPTION(scope, { });

    if (p1.isString()) {
        if (p2.isCell()) {
            JSString* p2String = p2.toString(globalObject);
            RETURN_IF_EXCEPTION(scope, { });
            RELEASE_AND_RETURN(scope, jsString(globalObject, asString(p1), p2String));
        }
        String p2String = p2.toWTFString(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, jsString(globalObject, asString(p1), p2String));
    }

    if (p2.isString()) {
        if (p1.isCell()) {
            JSString* p1String = p1.toString(globalObject);
            RETURN_IF_EXCEPTION(scope, { });
            RELEASE_AND_RETURN(scope, jsString(globalObject, p1String, asString(p2)));
        }
        String p1String = p1.toWTFString(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, jsString(globalObject, p1String, asString(p2)));
    }

    auto doubleOp = [] (double left, double right) -> double {
        return left + right;
    };

    auto bigIntOp = [] (JSGlobalObject* globalObject, auto left, auto right) {
        return JSBigInt::add(globalObject, left, right);
    };

    RELEASE_AND_RETURN(scope, arithmeticBinaryOp<OverloadableOperator::Add>(globalObject, p1, p2, doubleOp, bigIntOp, "Invalid mix of BigInt and other type in addition."_s));
}

// ---- String concatenation and overloaded operators. See Operations.h.

JSValue OperandsOfStringConcatenation::at(unsigned index) const
{
    return first[static_cast<int>(index) * stride].jsValue();
}

// What the operands up to and including the one that this is in place of come to, along with so many of those that follow it.
static bool isSumSoFar(JSValue value)
{
    return value.isCell() && value.asCell()->type() == InternalFieldTupleType;
}

static JSValue sumSoFar(JSGlobalObject* globalObject, JSValue sum, unsigned following)
{
    return InternalFieldTuple::create(globalObject->vm(), globalObject->internalFieldTupleStructure(), sum, jsNumber(following));
}

static JSValue valueOfSumSoFar(JSValue sum)
{
    return uncheckedDowncast<InternalFieldTuple>(sum.asCell())->internalField(InternalFieldTuple::Field::Slot0).get();
}

// ((a + b) + c) + ...
static JSValue addUp(JSGlobalObject* globalObject, OperandsOfStringConcatenation operands, unsigned count)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT(count);
    JSValue sum = operands.at(0);
    unsigned next = 1;
    for (unsigned i = count; i--;) {
        JSValue operand = operands.at(i);
        if (!isSumSoFar(operand))
            continue;
        sum = valueOfSumSoFar(operand);
        next = i + 1 + uncheckedDowncast<InternalFieldTuple>(operand.asCell())->internalField(InternalFieldTuple::Field::Slot1).get().asInt32();
        break;
    }
    for (; next < count; ++next) {
        sum = jsAdd(globalObject, sum, operands.at(next));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return sum;
}

JSValue toPrimitiveForStringConcatenation(JSGlobalObject* globalObject, OperandsOfStringConcatenation operands, unsigned index, unsigned previous, unsigned literalsAfter)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue operand = operands.at(index);
    bool hasSum = previous != index && isSumSoFar(operands.at(previous));
    if (!operand.isObject() && !hasSum)
        return operand;

    JSValue sum;
    if (hasSum) {
        sum = valueOfSumSoFar(operands.at(previous));
        // The second has been added to the first already, if the first is what it came to.
        if (previous || index != 1) {
            sum = jsAdd(globalObject, sum, operand);
            RETURN_IF_EXCEPTION(scope, { });
        }
    } else if (!index && !literalsAfter) {
        // The second is no literal and has been evaluated. This is when they are added. If it is the second that overloads operators, it is given the first as it is.
        JSValue second = operands.at(1);
        if (!operand.overloadsOperators() && !second.overloadsOperators())
            RELEASE_AND_RETURN(scope, operand.toPrimitive(globalObject));
        sum = jsAdd(globalObject, operand, second);
        RETURN_IF_EXCEPTION(scope, { });
        return sumSoFar(globalObject, sum, 1);
    } else {
        if (!operand.overloadsOperators())
            RELEASE_AND_RETURN(scope, operand.toPrimitive(globalObject));
        sum = operand;
        if (index) {
            JSValue left = addUp(globalObject, operands, index);
            RETURN_IF_EXCEPTION(scope, { });
            sum = jsAdd(globalObject, left, operand);
            RETURN_IF_EXCEPTION(scope, { });
        }
    }
    for (unsigned i = 0; i < literalsAfter; ++i) {
        sum = jsAdd(globalObject, sum, operands.at(index + 1 + i));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return sumSoFar(globalObject, sum, literalsAfter);
}

JSValue addUpInsteadOfConcatenating(JSGlobalObject* globalObject, OperandsOfStringConcatenation operands, unsigned count, unsigned firstOperand)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!firstOperand)
        RELEASE_AND_RETURN(scope, addUp(globalObject, operands, count));

    // d += a + b + c
    ASSERT(firstOperand == 1);
    JSValue left = operands.at(0);
    JSValue right = addUp(globalObject, { operands.first + operands.stride, operands.stride }, count - 1);
    RETURN_IF_EXCEPTION(scope, { });
    if (!left.overloadsOperators() && !right.overloadsOperators())
        RELEASE_AND_RETURN(scope, jsAdd(globalObject, left, right));
    JSCell* cell = left.overloadsOperators() ? left.asCell() : right.asCell();
    JSValue result = cell->methodTable()->operate(globalObject, OverloadableOperator::Add, left, right, true);
    RETURN_IF_EXCEPTION(scope, { });
    if (result)
        return result;
    RELEASE_AND_RETURN(scope, addWithoutOverloading(globalObject, left, right));
}

JSValue toPrimitiveForTargetOfStringConcatenation(JSGlobalObject* globalObject, JSValue target, JSValue previous)
{
    // It is left as it is for whichever of the two overloads operators to be given. The op_strcat comes next.
    if (target.overloadsOperators() || isSumSoFar(previous))
        return target;
    return target.toPrimitive(globalObject);
}

JSValue toPrimitiveForAdditionOfEmptyString(JSGlobalObject* globalObject, JSValue operand, unsigned addition)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (operand.overloadsOperators()) [[unlikely]] {
        JSValue empty = jsEmptyString(vm);
        bool isOnTheLeft = addition & EmptyStringIsOnTheLeft;
        JSValue result = operand.asCell()->methodTable()->operate(globalObject, OverloadableOperator::Add, isOnTheLeft ? empty : operand, isOnTheLeft ? operand : empty, addition & AdditionOfEmptyStringIsCompoundAssignment);
        RETURN_IF_EXCEPTION(scope, { });
        if (result)
            return sumSoFar(globalObject, result, 0);
    }
    RELEASE_AND_RETURN(scope, operand.toPrimitive(globalObject));
}

JSValue toStringForAdditionOfEmptyString(JSGlobalObject* globalObject, JSValue value)
{
    if (isSumSoFar(value))
        return valueOfSumSoFar(value);
    return value.toString(globalObject);
}

JSString* jsTypeStringForValueWithConcurrency(VM& vm, JSGlobalObject* globalObject, JSValue v, Concurrency concurrency)
{
    if (v.isUndefined())
        return vm.smallStrings.undefinedString();
    if (v.isBoolean())
        return vm.smallStrings.booleanString();
    if (v.isNumber())
        return vm.smallStrings.numberString();
    if (v.isString())
        return vm.smallStrings.stringString();
    if (v.isSymbol())
        return vm.smallStrings.symbolString();
    if (v.isBigInt())
        return vm.smallStrings.bigintString();
    if (v.isObject()) {
        JSObject* object = asObject(v);
        // Return "undefined" for objects that should be treated
        // as null when doing comparisons.
        if (object->structure()->masqueradesAsUndefined(globalObject))
            return vm.smallStrings.undefinedString();
        if (concurrency == Concurrency::MainThread) [[likely]] {
            if (object->isCallable())
                return vm.smallStrings.functionString();
            return vm.smallStrings.objectString();
        }

        switch (object->isCallableWithConcurrency<Concurrency::ConcurrentThread>()) {
        case TriState::True:
            return vm.smallStrings.functionString();
        case TriState::False:
            return vm.smallStrings.objectString();
        case TriState::Indeterminate:
            return nullptr;
        }
    }
    // This case can happen for internal objects like GetterSetter, that
    // can be exposed to this function by transformations like LICM blind hoisting.
    // The actual result shouldn't matter, as long as it matches buildTypeOf.
    return vm.smallStrings.objectString();
}

size_t normalizePrototypeChain(JSGlobalObject* globalObject, JSCell* base, bool& sawPolyProto)
{
    VM& vm = globalObject->vm();
    size_t count = 0;
    sawPolyProto = false;
    JSCell* current = base;
    while (1) {
        Structure* structure = current->structure();
        if (structure->isProxy())
            return InvalidPrototypeChain;

        sawPolyProto |= structure->hasPolyProto();

        JSValue prototype = structure->prototypeForLookup(globalObject, current);
        if (prototype.isNull())
            return count;

        current = prototype.asCell();
        structure = current->structure();
        if (structure->isDictionary()) {
            if (structure->hasBeenFlattenedBefore())
                return InvalidPrototypeChain;
            structure->flattenDictionaryStructure(vm, asObject(current));
        }

        ++count;
    }
}

} // namespace JSC
