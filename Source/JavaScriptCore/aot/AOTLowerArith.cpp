/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLowering.h"

#if ENABLE(AOT) && (CPU(ARM64) || CPU(X86_64))

#include "B3PatchpointValue.h"
#include "BytecodeStructs.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

using namespace B3;

static std::optional<Stub> stubFor(OpcodeID opcode)
{
    switch (opcode) {
    case op_add: return Stub::Add;
    case op_sub: return Stub::Sub;
    case op_mul: return Stub::Mul;
    case op_div: return Stub::Div;
    case op_mod: return Stub::Mod;
    case op_bitand: return Stub::BitAnd;
    case op_bitor: return Stub::BitOr;
    case op_bitxor: return Stub::BitXor;
    case op_lshift: return Stub::LShift;
    case op_rshift: return Stub::RShift;
    case op_urshift: return Stub::URShift;
    case op_less: return Stub::Less;
    case op_lesseq: return Stub::LessEq;
    case op_greater: return Stub::Greater;
    case op_greatereq: return Stub::GreaterEq;
    default: return std::nullopt;
    }
}

LValue Lowering::callBinaryStub(Node* node, Stub stub, LType type, LValue a, LValue b)
{
    return callStub(stub, type, { { a, firstStubOperandGPR }, { b, GPRInfo::argumentGPR1 } }, { }, StubClobbers::CallerSavedRegisters, node);
}

static Entry operationFor(OpcodeID opcode)
{
    switch (opcode) {
    case op_add: return Entry::operationAOTValueAdd;
    case op_sub: return Entry::operationAOTValueSub;
    case op_mul: return Entry::operationAOTValueMul;
    case op_div: return Entry::operationAOTValueDiv;
    case op_mod: return Entry::operationAOTValueMod;
    case op_pow: return Entry::operationAOTValuePow;
    case op_bitand: return Entry::operationAOTValueBitAnd;
    case op_bitor: return Entry::operationAOTValueBitOr;
    case op_bitxor: return Entry::operationAOTValueBitXor;
    case op_lshift: return Entry::operationAOTValueLShift;
    case op_rshift: return Entry::operationAOTValueRShift;
    case op_urshift: return Entry::operationAOTValueURShift;
    case op_negate: return Entry::operationAOTValueNegate;
    case op_bitnot: return Entry::operationAOTValueBitNot;
    case op_inc: return Entry::operationAOTValueInc;
    case op_dec: return Entry::operationAOTValueDec;
    case op_to_number: return Entry::operationAOTToNumber;
    case op_to_numeric: return Entry::operationAOTToNumeric;
    case op_to_string: return Entry::operationAOTToString;
    case op_less: return Entry::operationAOTCompareLess;
    case op_lesseq: return Entry::operationAOTCompareLessEq;
    case op_greater: return Entry::operationAOTCompareGreater;
    case op_greatereq: return Entry::operationAOTCompareGreaterEq;
    default:
        RELEASE_ASSERT_NOT_REACHED();
        return Entry::operationAOTValueAdd;
    }
}

void Lowering::lowerBinaryArith(Node* node, VirtualRegister lhs, VirtualRegister rhs)
{
    Node* left = node->use(lhs);
    Node* right = node->use(rhs);
    OpcodeID opcode = node->opcode;

    auto doubleOp = [&](LValue a, LValue b) -> LValue {
        switch (opcode) {
        case op_add: return m_out.doubleAdd(a, b);
        case op_sub: return m_out.doubleSub(a, b);
        case op_mul: return m_out.doubleMul(a, b);
        case op_div: return m_out.doubleDiv(a, b);
        case op_mod: return plainCall(Double, Entry::operationAOTFMod, a, b);
        case op_pow: return plainCall(Double, Entry::operationAOTPow, a, b);
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return nullptr;
        }
    };

    if (node->isInteger() && left->isInteger() && right->isInteger()) {
        m_graph.remark("integer-arithmetic"_s);
        bool narrow = node->rep() == Rep::Int32 && left->rep() == Rep::Int32 && right->rep() == Rep::Int32;
        LValue a = narrow ? lowInt32(left) : lowInt64(left);
        LValue b = narrow ? lowInt32(right) : lowInt64(right);
        LValue result;
        switch (opcode) {
        case op_add:
            result = m_out.add(a, b);
            break;
        case op_sub:
            result = m_out.sub(a, b);
            break;
        case op_mul:
            result = m_out.mul(a, b);
            break;
        case op_mod:
            result = m_out.sub(a, m_out.mul(m_out.div(a, b), b));
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
        setResult(node, result, narrow ? Rep::Int32 : Rep::Int64);
        return;
    }
    auto int32Remainder = [&](LValue a, LValue b, LBasicBlock otherwise) {
        orElse(m_out.bitAnd(m_out.greaterThanOrEqual(a, m_out.int32Zero), m_out.greaterThan(b, m_out.int32Zero)), otherwise);
        return m_out.sub(a, m_out.mul(m_out.div(a, b), b));
    };
    if (opcode == op_mod && left->rep() == Rep::Int32 && right->rep() == Rep::Int32) {
        LBasicBlock otherwise = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        LValue a = lowInt32(left);
        LValue b = lowInt32(right);
        ValueFromBlock quick = m_out.anchor(m_out.intToDouble(int32Remainder(a, b, otherwise)));
        m_out.jump(continuation);
        m_out.appendTo(otherwise);
        ValueFromBlock slow = m_out.anchor(doubleOp(m_out.intToDouble(a), m_out.intToDouble(b)));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        setDouble(node, m_out.phi(Double, quick, slow));
        return;
    }
    if (isSubtype(left->type | right->type, TNumber)) {
        setDouble(node, doubleOp(lowDouble(left), lowDouble(right)));
        return;
    }
    if (opcode == op_add && left->type && right->type && ((isSubtype(left->type, TString) && isSubtype(right->type, TNumber)) || (isSubtype(left->type, TNumber) && isSubtype(right->type, TString)))) {
        m_graph.remark("concatenates-string-and-number"_s);
        LValue first = isSubtype(left->type, TString) ? lowJSValue(left) : numberToString(node, left);
        LValue second = isSubtype(right->type, TString) ? lowJSValue(right) : numberToString(node, right);
        setJSValue(node, withHelper(Stub::HelperMakeRope2, { first, second }, [&] { return vmCall(node, pointerType(), Entry::operationMakeRope2, m_globalObject, first, second); }));
        return;
    }

    LValue a = lowJSValue(left);
    LValue b = lowJSValue(right);
    bool mayBeNumbers = mayBe(left->type, TNumber) && mayBe(right->type, TNumber);
    if (opcode == op_add && left->type && right->type && isSubtype(left->type | right->type, TString)) {
        setJSValue(node, withHelper(Stub::HelperMakeRope2, { a, b }, [&] { return vmCall(node, pointerType(), Entry::operationMakeRope2, m_globalObject, a, b); }));
        return;
    }
    if (!mayBeNumbers) {
        auto callOperation = [&] { return vmCall(node, Int64, operationFor(opcode), contextOf(operationFor(opcode)), a, b); };
        setJSValue(node, opcode == op_add && mayBe(left->type | right->type, TString) ? withHelper(Stub::HelperAddStrings, { a, b }, callOperation) : callOperation());
        return;
    }
    if (isCompact()) {
        if (opcode == op_add && mayBe(left->type, TString) && mayBe(right->type, TString))
            m_graph.remark("joins-strings-in-stub"_s);
        if (auto stub = stubFor(opcode))
            setJSValue(node, callBinaryStub(node, *stub, Int64, a, b));
        else
            setJSValue(node, vmCall(node, Int64, operationFor(opcode), contextOf(operationFor(opcode)), a, b));
        return;
    }

    LBasicBlock intCase = m_out.newBlock();
    LBasicBlock notBothInt = m_out.newBlock();
    LBasicBlock doubleCase = m_out.newBlock();
    LBasicBlock slowCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 3> results;

    bool hasIntCase = opcode == op_add || opcode == op_sub || opcode == op_mul || opcode == op_mod;
    if (hasIntCase)
        m_out.branch(m_out.bitAnd(isInt32(a), isInt32(b)), unsure(intCase), unsure(notBothInt));
    else
        m_out.jump(notBothInt);

    m_out.appendTo(intCase, notBothInt);
    if (opcode == op_mod) {
        results.append(m_out.anchor(boxInt32(int32Remainder(unboxInt32(a), unboxInt32(b), doubleCase))));
        m_out.jump(continuation);
    } else if (hasIntCase) {
        LValue isUTF16A = m_out.signExt32To64(unboxInt32(a));
        LValue isUTF16B = m_out.signExt32To64(unboxInt32(b));
        LValue wide = opcode == op_add ? m_out.add(isUTF16A, isUTF16B) : opcode == op_sub ? m_out.sub(isUTF16A, isUTF16B) : m_out.mul(isUTF16A, isUTF16B);
        LValue narrow = m_out.castToInt32(wide);
        LValue fits = m_out.equal(m_out.signExt32To64(narrow), wide);
        if (opcode == op_mul)
            fits = m_out.bitAnd(fits, m_out.bitOr(m_out.notZero32(narrow), m_out.greaterThanOrEqual(m_out.bitOr(isUTF16A, isUTF16B), m_out.constInt64(0))));
        results.append(m_out.anchor(boxInt32(narrow)));
        m_out.branch(fits, usually(continuation), rarely(doubleCase));
    } else
        m_out.unreachable();

    m_out.appendTo(notBothInt, doubleCase);
    m_out.branch(m_out.bitAnd(isNumber(a), isNumber(b)), unsure(doubleCase), unsure(slowCase));

    m_out.appendTo(doubleCase, slowCase);
    results.append(m_out.anchor(boxDouble(doubleOp(numberToDouble(a), numberToDouble(b)))));
    m_out.jump(continuation);

    m_out.appendTo(slowCase, continuation);
    if (opcode == op_add && mayBe(left->type, TString) && mayBe(right->type, TString)) {
        LBasicBlock notStrings = newColdBlock();
        results.append(m_out.anchor(addStrings(a, b, notStrings)));
        m_out.jump(continuation);
        m_out.appendTo(notStrings);
    }
    results.append(m_out.anchor(vmCall(node, Int64, operationFor(opcode), contextOf(operationFor(opcode)), a, b)));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    setJSValue(node, m_out.phi(Int64, results));
}

LValue Lowering::toInt32ForBitOp(Node* operand)
{
    switch (operand->rep()) {
    case Rep::Int32:
        return lowInt32(operand);
    case Rep::Int64:
        return m_out.castToInt32(lowRaw(operand));
    case Rep::Double:
        return doubleToInt32(lowDouble(operand));
    case Rep::Boolean:
        return lowBoolean(operand);
    case Rep::JSValue:
        break;
    }
    RELEASE_ASSERT(isSubtype(operand->type, TNumber | TBoolean));
    LValue value = lowRaw(operand);
    LValue result = unboxBoolean(value);
    if (mayBe(operand->type, TInt32))
        result = m_out.select(isInt32(value), unboxInt32(value), result);
    if (!mayBe(operand->type, TDouble))
        return result;

    m_graph.remark("double-to-int32-behind-branch"_s);
    LBasicBlock doubleCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    ValueFromBlock withoutConversion = m_out.anchor(result);
    LValue isDouble = mayBe(operand->type, TInt32) ? m_out.bitAnd(isNumber(value), isNotInt32(value)) : isNumber(value);
    m_out.branch(isDouble, unsure(doubleCase), unsure(continuation));
    m_out.appendTo(doubleCase);
    ValueFromBlock converted = m_out.anchor(doubleToInt32(unboxDouble(value)));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return m_out.phi(Int32, withoutConversion, converted);
}

void Lowering::lowerBitOp(Node* node, VirtualRegister lhs, VirtualRegister rhs)
{
    Node* left = node->use(lhs);
    Node* right = node->use(rhs);
    OpcodeID opcode = node->opcode;

    auto intOp = [&](LValue a, LValue b) -> LValue {
        switch (opcode) {
        case op_bitand: return m_out.bitAnd(a, b);
        case op_bitor: return m_out.bitOr(a, b);
        case op_bitxor: return m_out.bitXor(a, b);
        case op_lshift: return m_out.shl(a, m_out.bitAnd(b, m_out.constInt32(31)));
        case op_rshift: return m_out.aShr(a, m_out.bitAnd(b, m_out.constInt32(31)));
        case op_urshift: return m_out.lShr(a, m_out.bitAnd(b, m_out.constInt32(31)));
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return nullptr;
        }
    };
    auto finish = [&](LValue result) {
        setInt32(node, result);
    };

    if (isSubtype(left->type | right->type, TNumber | TBoolean)) {
        finish(intOp(toInt32ForBitOp(left), toInt32ForBitOp(right)));
        return;
    }

    LValue a = lowJSValue(left);
    LValue b = lowJSValue(right);
    auto asInt32 = [&](Node* operand, LValue boxed) -> LValue {
        return operand->rep() == Rep::Int32 ? lowInt32(operand) : unboxInt32(boxed);
    };
    if (bool leftIsInt32 = isSubtype(left->type, TInt32); (leftIsInt32 || isSubtype(right->type, TInt32)) && isSubtype(node->type, TInt32)) {
        m_graph.remark("inline-bit-operation"_s);
        LBasicBlock intCase = m_out.newBlock();
        LBasicBlock slowCase = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        m_out.branch(isInt32(leftIsInt32 ? b : a), usually(intCase), rarely(slowCase));

        m_out.appendTo(intCase);
        ValueFromBlock fastResult = m_out.anchor(intOp(asInt32(left, a), asInt32(right, b)));
        m_out.jump(continuation);

        m_out.appendTo(slowCase);
        LValue slow = isCompact() ? callBinaryStub(node, *stubFor(opcode), Int64, a, b) : vmCall(node, Int64, operationFor(opcode), contextOf(operationFor(opcode)), a, b);
        ValueFromBlock slowResult = m_out.anchor(unboxInt32(slow));
        m_out.jump(continuation);

        m_out.appendTo(continuation);
        finish(m_out.phi(Int32, fastResult, slowResult));
        return;
    }
    if (isCompact()) {
        setJSValue(node, callBinaryStub(node, *stubFor(opcode), Int64, a, b));
        return;
    }
    LBasicBlock intCase = m_out.newBlock();
    LBasicBlock slowCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(m_out.bitAnd(isInt32(a), isInt32(b)), usually(intCase), rarely(slowCase));

    m_out.appendTo(intCase, slowCase);
    LValue fast = intOp(asInt32(left, a), asInt32(right, b));
    ValueFromBlock fastResult = m_out.anchor(boxInt32(fast));
    m_out.jump(continuation);

    m_out.appendTo(slowCase, continuation);
    ValueFromBlock slowResult = m_out.anchor(vmCall(node, Int64, operationFor(opcode), contextOf(operationFor(opcode)), a, b));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    setJSValue(node, m_out.phi(Int64, fastResult, slowResult));
}

LValue Lowering::numberToString(Node* origin, Node* number)
{
    if (number->rep() != Rep::Int32)
        return vmCall(origin, pointerType(), Entry::operationDoubleToStringWithValidRadix, m_globalObject, lowDouble(number), m_out.constInt32(10));
    LValue value = lowInt32(number);
    return withHelper(Stub::HelperInt32ToString, { value }, [&] {
        return vmCall(origin, pointerType(), Entry::operationInt32ToStringWithValidRadix, m_globalObject, value, m_out.constInt32(10));
    });
}

void Lowering::lowerUnaryArith(Node* node, VirtualRegister operandRegister)
{
    Node* operand = node->use(operandRegister);
    OpcodeID opcode = node->opcode;

    if (node->isInteger() && operand->isInteger() && (opcode == op_inc || opcode == op_dec || opcode == op_negate)) {
        if (opcode == op_negate) {
            setInt64(node, m_out.neg(lowInt64(operand)));
            return;
        }
        int64_t step = opcode == op_inc ? 1 : -1;
        if (node->rep() == Rep::Int32 && operand->rep() == Rep::Int32) {
            setInt32(node, m_out.add(lowInt32(operand), m_out.constInt32(static_cast<int32_t>(step))));
            return;
        }
        LValue value = lowInt64(operand);
        LValue result = m_out.add(value, m_out.constInt64(step));
        int64_t end = step * IntegerRange::limit;
        if (operand->range.contains(end))
            result = m_out.select(m_out.equal(value, m_out.constInt64(end)), value, result);
        setInt64(node, result);
        return;
    }
    if (isSubtype(operand->type, TNumber)) {
        switch (opcode) {
        case op_inc:
            setDouble(node, m_out.doubleAdd(lowDouble(operand), m_out.constDouble(1)));
            return;
        case op_dec:
            setDouble(node, m_out.doubleSub(lowDouble(operand), m_out.constDouble(1)));
            return;
        case op_negate:
            setDouble(node, m_out.doubleNeg(lowDouble(operand)));
            return;
        case op_bitnot:
            setInt32(node, m_out.bitNot(toInt32ForBitOp(operand)));
            return;
        case op_to_number:
        case op_to_numeric:
            setResult(node, lowRaw(operand), operand->rep());
            return;
        default:
            break;
        }
    }
    if (opcode == op_to_string && isSubtype(operand->type, TString)) {
        setJSValue(node, lowJSValue(operand));
        return;
    }
    if (opcode == op_to_string && operand->type && isSubtype(operand->type, TNumber)) {
        m_graph.remark("number-to-string-by-type"_s);
        setJSValue(node, numberToString(node, operand));
        return;
    }

    LValue value = lowJSValue(operand);
    LBasicBlock fastCase = m_out.newBlock();
    LBasicBlock slowCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 2> results;

    switch (opcode) {
    case op_inc:
    case op_dec: {
        int32_t limit = opcode == op_inc ? INT32_MAX : INT32_MIN;
        LBasicBlock notInt32Case = m_out.newBlock();
        LBasicBlock doubleCase = m_out.newBlock();
        m_out.branch(m_out.bitAnd(isInt32(value), m_out.notEqual(unboxInt32(value), m_out.constInt32(limit))), usually(fastCase), rarely(notInt32Case));
        m_out.appendTo(notInt32Case, doubleCase);
        m_out.branch(isNumber(value), usually(doubleCase), rarely(slowCase));
        m_out.appendTo(doubleCase, fastCase);
        results.append(m_out.anchor(boxDouble(m_out.doubleAdd(numberToDouble(value), m_out.constDouble(opcode == op_inc ? 1 : -1)))));
        m_out.jump(continuation);
        m_out.appendTo(fastCase, slowCase);
        results.append(m_out.anchor(boxInt32(m_out.add(unboxInt32(value), m_out.constInt32(opcode == op_inc ? 1 : -1)))));
        break;
    }
    case op_negate:
        m_out.branch(m_out.bitAnd(isInt32(value), m_out.testNonZero32(unboxInt32(value), m_out.constInt32(INT32_MAX))), usually(fastCase), rarely(slowCase));
        m_out.appendTo(fastCase, slowCase);
        results.append(m_out.anchor(boxInt32(m_out.neg(unboxInt32(value)))));
        break;
    case op_bitnot:
        m_out.branch(isInt32(value), usually(fastCase), rarely(slowCase));
        m_out.appendTo(fastCase, slowCase);
        results.append(m_out.anchor(boxInt32(m_out.bitNot(unboxInt32(value)))));
        break;
    case op_to_number:
    case op_to_numeric:
        m_out.branch(isNumber(value), usually(fastCase), rarely(slowCase));
        m_out.appendTo(fastCase, slowCase);
        results.append(m_out.anchor(value));
        break;
    case op_to_string: {
        LBasicBlock cellCase = m_out.newBlock();
        m_out.branch(isCell(value), usually(cellCase), rarely(slowCase));
        m_out.appendTo(cellCase, fastCase);
        m_out.branch(isCellOfType(value, StringType), usually(fastCase), rarely(slowCase));
        m_out.appendTo(fastCase, slowCase);
        results.append(m_out.anchor(value));
        break;
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    m_out.jump(continuation);

    m_out.appendTo(slowCase, continuation);
    results.append(m_out.anchor(vmCall(node, Int64, operationFor(opcode), contextOf(operationFor(opcode)), value)));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    setJSValue(node, m_out.phi(Int64, results));
}

LValue Lowering::lowerCompare(Node* node, OpcodeID opcode, VirtualRegister lhs, VirtualRegister rhs)
{
    Node* left = node->use(lhs);
    Node* right = node->use(rhs);

    auto intCompare = [&](LValue a, LValue b) -> LValue {
        switch (opcode) {
        case op_less: return m_out.lessThan(a, b);
        case op_lesseq: return m_out.lessThanOrEqual(a, b);
        case op_greater: return m_out.greaterThan(a, b);
        case op_greatereq: return m_out.greaterThanOrEqual(a, b);
        case op_below: return m_out.below(a, b);
        case op_beloweq: return m_out.belowOrEqual(a, b);
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return nullptr;
        }
    };
    auto doubleCompare = [&](LValue a, LValue b) -> LValue {
        switch (opcode) {
        case op_less: return m_out.doubleLessThan(a, b);
        case op_lesseq: return m_out.doubleLessThanOrEqual(a, b);
        case op_greater: return m_out.doubleGreaterThan(a, b);
        case op_greatereq: return m_out.doubleGreaterThanOrEqual(a, b);
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return nullptr;
        }
    };

    if (opcode == op_below || opcode == op_beloweq) {
        LValue a = left->rep() == Rep::Int32 ? lowInt32(left) : unboxInt32(lowJSValue(left));
        LValue b = right->rep() == Rep::Int32 ? lowInt32(right) : unboxInt32(lowJSValue(right));
        return intCompare(a, b);
    }
    if (left->rep() == Rep::Int32 && right->rep() == Rep::Int32)
        return intCompare(lowInt32(left), lowInt32(right));
    if (left->isInteger() && right->isInteger())
        return intCompare(lowInt64(left), lowInt64(right));
    if (isSubtype(left->type | right->type, TNumber))
        return doubleCompare(lowDouble(left), lowDouble(right));
    if (isSubtype((left->type | right->type) & ~TEmpty, TNumber | TUndefined)) {
        m_graph.remark("inline-comparison-of-numbers-or-undefined"_s);
        auto asDouble = [&](Node* operand) {
            return isSubtype(operand->type, TNumber) ? lowDouble(operand) : numberToDouble(lowJSValue(operand));
        };
        LValue leftDouble = asDouble(left);
        return doubleCompare(leftDouble, asDouble(right));
    }

    LValue a = lowJSValue(left);
    LValue b = lowJSValue(right);
    if (isCompact()) {
        bool leftIsInt32 = isSubtype(left->type, TInt32);
        if (!leftIsInt32 && !isSubtype(right->type, TInt32))
            return callBinaryStub(node, *stubFor(opcode), Int32, a, b);
        m_graph.remark("inline-relational-comparison-with-int32"_s);
        LBasicBlock intCase = m_out.newBlock();
        LBasicBlock slowCase = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        m_out.branch(isInt32(leftIsInt32 ? b : a), usually(intCase), rarely(slowCase));
        m_out.appendTo(intCase);
        ValueFromBlock fastResult = m_out.anchor(intCompare(unboxInt32(a), unboxInt32(b)));
        m_out.jump(continuation);
        m_out.appendTo(slowCase);
        ValueFromBlock slowResult = m_out.anchor(callBinaryStub(node, *stubFor(opcode), Int32, a, b));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return m_out.phi(Int32, fastResult, slowResult);
    }
    LBasicBlock intCase = m_out.newBlock();
    LBasicBlock notBothInt = m_out.newBlock();
    LBasicBlock doubleCase = m_out.newBlock();
    LBasicBlock slowCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 3> results;

    m_out.branch(m_out.bitAnd(isInt32(a), isInt32(b)), unsure(intCase), unsure(notBothInt));
    m_out.appendTo(intCase, notBothInt);
    results.append(m_out.anchor(intCompare(unboxInt32(a), unboxInt32(b))));
    m_out.jump(continuation);

    m_out.appendTo(notBothInt, doubleCase);
    m_out.branch(m_out.bitAnd(isNumber(a), isNumber(b)), unsure(doubleCase), unsure(slowCase));
    m_out.appendTo(doubleCase, slowCase);
    results.append(m_out.anchor(doubleCompare(numberToDouble(a), numberToDouble(b))));
    m_out.jump(continuation);

    m_out.appendTo(slowCase, continuation);
    results.append(m_out.anchor(m_out.notZero64(vmCall(node, Int64, operationFor(opcode), contextOf(operationFor(opcode)), a, b))));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    return m_out.phi(Int32, results);
}

std::optional<String> Lowering::constantStringOf(Node* node)
{
    if (node->kind != NodeKind::ConstantCell || !node->reg.isConstant())
        return std::nullopt;
    JSValue constant = node->codeBlockOfConstant()->getConstant(node->reg);
    if (!constant || !constant.isString())
        return std::nullopt;
    String said = asString(constant)->tryGetValue();
    if (said.isNull() || !said.is8Bit() || said.length() > TypedLayoutTable::maxAtomizedStringLength)
        return std::nullopt;
    return said;
}

bool Lowering::isAtomIfString(Node* node, unsigned depth)
{
    if (node->type && !mayBe(node->type, TOtherString))
        return true;
    if (depth > 4)
        return false;
    switch (node->kind) {
    case NodeKind::Narrow:
        return isAtomIfString(node->uses[0].node, depth + 1);
    case NodeKind::Phi:
        for (auto& use : node->uses) {
            if (use.node != node && !isAtomIfString(use.node, depth + 1))
                return false;
        }
        return true;
    case NodeKind::Bytecode:
        if (node->opcode == op_check_type)
            return isAtomIfString(node->use(node->as<OpCheckType>().m_value), depth + 1);
        if (node->opcode == op_check_tdz)
            return isAtomIfString(node->uses[0].node, depth + 1);
        return false;
    default:
        return false;
    }
}

bool Lowering::isAtomIfShortString(Node* node, unsigned depth)
{
    if (node->type && !mayBe(node->type, TShortOtherString))
        return true;
    if (depth > 4)
        return false;
    switch (node->kind) {
    case NodeKind::Narrow:
        return isAtomIfShortString(node->uses[0].node, depth + 1);
    case NodeKind::Phi:
        for (auto& use : node->uses) {
            if (use.node != node && !isAtomIfShortString(use.node, depth + 1))
                return false;
        }
        return true;
    case NodeKind::Bytecode:
        if (node->opcode == op_check_type)
            return isAtomIfShortString(node->use(node->as<OpCheckType>().m_value), depth + 1);
        if (node->opcode == op_check_tdz)
            return isAtomIfShortString(node->uses[0].node, depth + 1);
        if (node->opcode == op_get_by_id) {
            auto field = Graph::typedFieldAccessedBy(node);
            return field && field->fieldType.atoms;
        }
        return false;
    default:
        return false;
    }
}

LValue Lowering::areEqualAssumingAtomStrings(Node* left, LValue a, Node* right, LValue b)
{
    auto implOf = [&](LValue string) { return m_out.loadPtr(string, m_heaps.JSRopeString_fiber0); };
    if (isSubtype(left->type, TString) && isSubtype(right->type, TString))
        return m_out.equal(implOf(a), implOf(b));
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 5> results;
    auto continueIf = [&](LValue condition) {
        LBasicBlock next = m_out.newBlock();
        results.append(m_out.anchor(m_out.equal(a, b)));
        m_out.branch(condition, unsure(next), unsure(continuation));
        m_out.appendTo(next);
    };
    for (auto [node, value] : { std::pair { left, a }, std::pair { right, b } }) {
        if (isSubtype(node->type, TString))
            continue;
        if (!isSubtype(node->type, TCell))
            continueIf(isCell(value));
        if (!isSubtype(node->type & TCell, TString))
            continueIf(m_out.equal(cellType(value), m_out.constInt32(StringType)));
    }
    results.append(m_out.anchor(m_out.equal(implOf(a), implOf(b))));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return m_out.phi(Int32, results);
}

void Lowering::atomizeIfString(Node* node, LValue value)
{
    if (!mayBe(node->type, TString) || isAtomIfShortString(node))
        return;
    LBasicBlock isNot = newColdBlock();
    LBasicBlock done = m_out.newBlock();
    auto continueIf = [&](LValue condition) {
        LBasicBlock next = m_out.newBlock();
        m_out.branch(condition, unsure(next), unsure(done));
        m_out.appendTo(next);
    };
    if (!isSubtype(node->type, TCell))
        continueIf(isCell(value));
    if (!isSubtype(node->type & TCell, TString))
        continueIf(m_out.equal(cellType(value), m_out.constInt32(StringType)));
    m_out.branch(m_out.testNonZero32(m_out.load8ZeroExt32(value, m_heaps.JSCell_typeInfoFlags), m_out.constInt32(TypeInfoPerCellBit)), usually(done), rarely(isNot));
    m_out.appendTo(isNot);
    plainCall(Void, Entry::operationAOTMakeAtom, value);
    m_out.jump(done);
    m_out.appendTo(done);
}

LValue Lowering::compareWithLiteral(LValue characters, std::span<const Latin1Character> written)
{
    LValue difference = m_out.int64Zero;
    for (unsigned at = 0; at < written.size();) {
        unsigned left = written.size() - at;
        unsigned width = left >= 8 ? 8 : left >= 4 ? 4 : left >= 2 ? 2 : 1;
        uint64_t expected = 0;
        memcpy(&expected, written.data() + at, width);
        m_graph.wideIntegerConstants.add(static_cast<int64_t>(expected));
        TypedPointer address = m_out.address(m_heaps.characters8.atAnyIndex(), characters, at);
        LValue loaded = width == 8 ? m_out.load64(address) : m_out.zeroExt(width == 4 ? m_out.load32(address) : width == 2 ? m_out.load16ZeroExt32(address) : m_out.load8ZeroExt32(address), Int64);
        difference = m_out.bitOr(difference, m_out.bitXor(loaded, m_out.constInt64(expected)));
        at += width;
    }
    return difference;
}

Lowering::Latin1Characters Lowering::latin1CharactersOf(LValue string, LBasicBlock otherwise, Vector<ValueFromBlock, 2>& lengthOtherwise)
{
    if (isCompact()) {
        PatchpointValue* both = callStub(Stub::Latin1Characters, Int64, { { string, firstStubOperandGPR } }, { }, StubClobbers::Temporaries);
        both->effects = Effects::none();
        both->effects.reads = HeapRange::top();
        both->effects.controlDependent = true;
        LValue characters = m_out.bitAnd(both, m_out.constInt64((1ll << 48) - 1));
        LValue length = m_out.castToInt32(m_out.lShr(both, m_out.constInt32(48)));
        LBasicBlock continuation = m_out.newBlock();
        lengthOtherwise.append(m_out.anchor(length));
        m_out.branch(m_out.notZero64(characters), usually(continuation), rarely(otherwise));
        m_out.appendTo(continuation);
        return { characters, length };
    }
    LBasicBlock contiguousCase = m_out.newBlock();
    LBasicBlock narrow = m_out.newBlock();
    LBasicBlock wide = m_out.newBlock();
    LBasicBlock rope = m_out.newBlock();
    LBasicBlock slice = m_out.newBlock();
    LBasicBlock narrowSlice = m_out.newBlock();
    LBasicBlock isUnresolvedRope = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    LValue fiber = m_out.loadPtr(string, m_heaps.JSRopeString_fiber0);
    m_out.branch(m_out.testNonZeroPtr(fiber, m_out.constIntPtr(JSString::isRopeInPointer)), rarely(rope), usually(contiguousCase));

    m_out.appendTo(contiguousCase);
    LValue implLength = m_out.load32(fiber, m_heaps.StringImpl_length);
    m_out.branch(m_out.testNonZero32(m_out.load32(fiber, m_heaps.StringImpl_hashAndFlags), m_out.constInt32(StringImpl::flagIs8Bit())), usually(narrow), rarely(wide));
    m_out.appendTo(narrow);
    ValueFromBlock implCharacters = m_out.anchor(m_out.loadPtr(fiber, m_heaps.StringImpl_data));
    ValueFromBlock latin1Length = m_out.anchor(implLength);
    m_out.jump(continuation);
    m_out.appendTo(wide);
    lengthOtherwise.append(m_out.anchor(implLength));
    m_out.jump(otherwise);

    m_out.appendTo(rope);
    LValue ropeLength = m_out.load32(string, m_heaps.JSRopeString_length);
    constexpr uintptr_t latin1SubstringBits = JSRopeString::isSubstringInPointer | JSRopeString::is8BitInPointer;
    m_out.branch(m_out.equal(m_out.bitAnd(fiber, m_out.constIntPtr(latin1SubstringBits)), m_out.constIntPtr(latin1SubstringBits)), unsure(slice), unsure(isUnresolvedRope));
    m_out.appendTo(slice);
    LValue lengthAndBaseLowBits = m_out.load64(string, m_heaps.JSRopeString_fiber1);
    LValue baseHighBitsAndOffset = m_out.load64(string, m_heaps.JSRopeString_fiber2);
    LValue base = m_out.bitOr(m_out.lShr(lengthAndBaseLowBits, m_out.constInt32(32)), m_out.shl(m_out.bitAnd(baseHighBitsAndOffset, m_out.constInt64(0xffff)), m_out.constInt32(32)));
    LValue offset = m_out.lShr(baseHighBitsAndOffset, m_out.constInt32(16));
    LValue baseImpl = m_out.loadPtr(base, m_heaps.JSString_value);
    m_out.branch(m_out.testNonZero32(m_out.load32(baseImpl, m_heaps.StringImpl_hashAndFlags), m_out.constInt32(StringImpl::flagIs8Bit())), usually(narrowSlice), rarely(isUnresolvedRope));
    m_out.appendTo(narrowSlice);
    ValueFromBlock sliceCharacters = m_out.anchor(m_out.add(m_out.loadPtr(baseImpl, m_heaps.StringImpl_data), offset));
    ValueFromBlock sliceLength = m_out.anchor(ropeLength);
    m_out.jump(continuation);
    m_out.appendTo(isUnresolvedRope);
    lengthOtherwise.append(m_out.anchor(ropeLength));
    m_out.jump(otherwise);

    m_out.appendTo(continuation);
    return { m_out.phi(pointerType(), implCharacters, sliceCharacters), m_out.phi(Int32, latin1Length, sliceLength) };
}

LValue Lowering::isStringEqualTo(Node* comparison, Node* valueNode, LValue value, const String& said, Node* literalString)
{
    LBasicBlock continuation = m_out.newBlock();
    LBasicBlock slowCase = newColdBlock();
    Vector<ValueFromBlock, 6> results;
    auto resolveIf = [&](LValue condition, bool answer, bool isLikely = false) {
        LBasicBlock next = m_out.newBlock();
        results.append(m_out.anchor(answer ? m_out.booleanTrue : m_out.booleanFalse));
        m_out.branch(condition, isLikely ? usually(continuation) : unsure(continuation), unsure(next));
        m_out.appendTo(next);
    };
    if (!isSubtype(valueNode->type, TCell))
        resolveIf(isNotCell(value), false);
    if (!isSubtype(valueNode->type, TString | ~TCell))
        resolveIf(m_out.notEqual(cellType(value), m_out.constInt32(StringType)), false);
    LBasicBlock needsSlowPath = m_out.newBlock();
    Vector<ValueFromBlock, 2> lengthsOtherwise;
    auto [characters, length] = latin1CharactersOf(value, needsSlowPath, lengthsOtherwise);
    resolveIf(m_out.notEqual(length, m_out.constInt32(said.length())), false, true);
    results.append(m_out.anchor(m_out.isZero64(compareWithLiteral(characters, said.span8()))));
    m_out.jump(continuation);

    m_out.appendTo(needsSlowPath);
    results.append(m_out.anchor(m_out.booleanFalse));
    m_out.branch(m_out.notEqual(m_out.phi(Int32, lengthsOtherwise), m_out.constInt32(said.length())), usually(continuation), rarely(slowCase));

    m_out.appendTo(slowCase);
    results.append(m_out.anchor(m_out.notZero64(vmCall(comparison, Int64, Entry::operationAOTCompareStrictEq, m_instance, value, lowJSValue(literalString)))));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    return m_out.phi(Int32, results);
}

LValue Lowering::isStringEqualToAtom(Node* valueNode, LValue value, LValue literalString)
{
    LBasicBlock continuation = m_out.newBlock();
    LBasicBlock isResolved = m_out.newBlock();
    LBasicBlock throughStub = newColdBlock();
    Vector<ValueFromBlock, 5> results;
    auto resolveIf = [&](LValue condition, bool answer) {
        LBasicBlock next = m_out.newBlock();
        results.append(m_out.anchor(answer ? m_out.booleanTrue : m_out.booleanFalse));
        m_out.branch(condition, unsure(continuation), unsure(next));
        m_out.appendTo(next);
    };
    if (!isSubtype(valueNode->type, TCell))
        resolveIf(isNotCell(value), false);
    if (!isSubtype(valueNode->type, TString | ~TCell))
        resolveIf(m_out.notEqual(cellType(value), m_out.constInt32(StringType)), false);
    LValue impl = m_out.loadPtr(value, m_heaps.JSRopeString_fiber0);
    resolveIf(m_out.equal(impl, m_out.loadPtr(literalString, m_heaps.JSRopeString_fiber0)), true);
    m_out.branch(m_out.testNonZeroPtr(impl, m_out.constIntPtr(JSString::isRopeInPointer)), rarely(throughStub), usually(isResolved));

    m_out.appendTo(isResolved);
    results.append(m_out.anchor(m_out.booleanFalse));
    m_out.branch(m_out.testNonZero32(m_out.load32(impl, m_heaps.StringImpl_hashAndFlags), m_out.constInt32(StringImpl::flagIsAtom())), usually(continuation), rarely(throughStub));

    m_out.appendTo(throughStub);
    results.append(m_out.anchor(callStub(Stub::IsStringEqualTo, Int32, { { value, firstStubOperandGPR }, { literalString, GPRInfo::argumentGPR1 } }, { })));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    return m_out.phi(Int32, results);
}

LValue Lowering::lowerEquality(Node* node, bool strict, VirtualRegister lhs, VirtualRegister rhs)
{
    Node* left = node->use(lhs);
    Node* right = node->use(rhs);
    if (Node* read = left->isElided ? left : right->isElided ? right : nullptr) {
        RELEASE_ASSERT(strict && Graph::isArrayIteratorMethodRead(read));
        LValue array = lowJSValue(read->use(read->as<OpGetById>().m_base));
        LBasicBlock isNotOriginalArray = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        ValueFromBlock trueResult = m_out.anchor(m_out.booleanTrue);
        m_out.branch(isOriginalArray(array), usually(continuation), rarely(isNotOriginalArray));
        m_out.appendTo(isNotOriginalArray);
        ValueFromBlock asked = m_out.anchor(m_out.equal(vmCall(read, Int64, Entry::operationAOTArrayIteratorMethod, m_instance, array), lowJSValue(read == left ? right : left)));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return m_out.phi(Int32, trueResult, asked);
    }
    Type both = left->type | right->type;

    if (left->rep() == Rep::Int32 && right->rep() == Rep::Int32)
        return m_out.equal(lowInt32(left), lowInt32(right));
    if (left->isInteger() && right->isInteger())
        return m_out.equal(lowInt64(left), lowInt64(right));
    if (isSubtype(both, TNumber))
        return m_out.doubleEqual(lowDouble(left), lowDouble(right));
    if (isSubtype(both, TBoolean))
        return m_out.equal(lowBoolean(left), lowBoolean(right));
    if (strict && !(left->type & right->type) && !(mayBe(left->type, TNumber) && mayBe(right->type, TNumber)))
        return m_out.booleanFalse;

    Type byContent = TNumber | TString | TBigInt;
    auto bothMayBe = [&](Type kind) { return mayBe(left->type, kind) && mayBe(right->type, kind); };
    bool areAtomStrings = (strict || isSubtype(both, TString)) && !mayBe(both, TNumber | TBigInt)
        && ((isAtomIfString(left) && isAtomIfString(right))
            || (constantStringOf(right) && isAtomIfString(right) && isAtomIfShortString(left))
            || (constantStringOf(left) && isAtomIfString(left) && isAtomIfShortString(right)));
    bool bitsDecide = strict ? !bothMayBe(TNumber) && !bothMayBe(TString) && !bothMayBe(TBigInt) : isSubtype(both, TAnyObject) || isSubtype(both, TSymbol) || isSubtype(both, TBoolean);
    if (!bitsDecide && !areAtomStrings && !isCompact() && (strict || isSubtype(both, TString))) {
        if (auto said = constantStringOf(right))
            return isStringEqualTo(node, left, lowJSValue(left), *said, right);
        if (auto said = constantStringOf(left))
            return isStringEqualTo(node, right, lowJSValue(right), *said, left);
    }

    if (!bitsDecide && isCompact() && !(m_block->isInLoop && !m_block->isGeneric) && (strict || isSubtype(both, TString))) {
        for (auto [literal, other] : { std::pair { right, left }, std::pair { left, right } }) {
            auto said = constantStringOf(literal);
            if (said && !said->isEmpty() && said->is8Bit() && said->length() <= 16) {
                auto characters = said->span8();
                unsigned length = characters.size();
                auto chunk = [&](unsigned start, unsigned size) {
                    uint64_t result = 0;
                    for (unsigned i = 0; i < size; ++i)
                        result |= static_cast<uint64_t>(characters[start + i]) << (8 * i);
                    return result;
                };
                m_graph.remark("short-literal-comparison"_s, *said);
                Stub stub = Stub::IsStringEqualToLiteral8;
                uint64_t bits = 0;
                if (length == 1) {
                    stub = Stub::IsStringEqualToLiteral1;
                    bits = chunk(0, 1);
                } else if (length <= 3) {
                    stub = Stub::IsStringEqualToLiteral2To3;
                    bits = chunk(0, 2) | chunk(length - 2, 2) << 16;
                } else if (length <= 7) {
                    stub = Stub::IsStringEqualToLiteral4To7;
                    bits = chunk(0, 4) | chunk(length - 4, 4) << 32;
                } else
                    bits = chunk(0, 8);
                m_graph.wideIntegerConstants.add(static_cast<int64_t>(bits));
                Vector<StubArgument, 8> operands { { lowJSValue(other), firstStubOperandGPR }, { m_out.constInt64(bits), GPRInfo::argumentGPR1 } };
                if (length > 8) {
                    stub = Stub::IsStringEqualToLiteral9To16;
                    uint64_t lastBits = chunk(length - 8, 8);
                    m_graph.wideIntegerConstants.add(static_cast<int64_t>(lastBits));
                    operands.append(StubArgument { m_out.constInt64(lastBits), GPRInfo::argumentGPR3 });
                }
                return callStub(stub, Int32, operands, { { stubImmediateGPR, programConstantIndex(literal) << shortLiteralLengthBits | (length > 8 ? length - 1 : length) } });
            }
            if (said && said->length() > 16)
                return callStub(Stub::IsStringEqualToConstant, Int32, { { lowJSValue(other), firstStubOperandGPR } }, { { stubImmediateGPR, programConstantIndex(literal) } });
        }
    }

    LValue a = lowJSValue(left);
    LValue b = lowJSValue(right);

    if (bitsDecide) {
        if (strict && bothMayBe(byContent))
            m_graph.remark("strict-equality-of-different-kinds-by-bits"_s);
        return m_out.equal(a, b);
    }

    if ((strict || isSubtype(both, TString)) && !mayBe(both, TNumber | TBigInt)) {
        if (areAtomStrings)
            return areEqualAssumingAtomStrings(left, a, right, b);
    }

    if (bool leftIsInt32 = isSubtype(left->type, TInt32); strict && (leftIsInt32 || isSubtype(right->type, TInt32))) {
        m_graph.remark("inline-comparison-with-int32"_s);
        LValue other = leftIsInt32 ? b : a;
        LBasicBlock differs = m_out.newBlock();
        LBasicBlock isDouble = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        Vector<ValueFromBlock, 3> results;
        results.append(m_out.anchor(m_out.booleanTrue));
        m_out.branch(m_out.equal(a, b), unsure(continuation), unsure(differs));

        m_out.appendTo(differs);
        results.append(m_out.anchor(m_out.booleanFalse));
        m_out.branch(m_out.bitAnd(isNumber(other), isNotInt32(other)), rarely(isDouble), usually(continuation));

        m_out.appendTo(isDouble);
        results.append(m_out.anchor(m_out.doubleEqual(unboxDouble(other), m_out.intToDouble(unboxInt32(leftIsInt32 ? a : b)))));
        m_out.jump(continuation);

        m_out.appendTo(continuation);
        return m_out.phi(Int32, results);
    }

    if (isCompact()) {
        if (strict || isSubtype(both, TString)) {
            bool isInline = m_block->isInLoop && !m_block->isGeneric;
            if (constantStringOf(right) && isAtomIfString(right))
                return isInline ? isStringEqualToAtom(left, a, b) : callStub(Stub::IsStringEqualTo, Int32, { { a, firstStubOperandGPR }, { b, GPRInfo::argumentGPR1 } }, { });
            if (constantStringOf(left) && isAtomIfString(left))
                return isInline ? isStringEqualToAtom(right, b, a) : callStub(Stub::IsStringEqualTo, Int32, { { b, firstStubOperandGPR }, { a, GPRInfo::argumentGPR1 } }, { });
        }
        return callStub(strict ? Stub::StrictEqual : Stub::LooseEqual, Int32, { { a, firstStubOperandGPR }, { b, GPRInfo::argumentGPR1 } },
            { });
    }

    LBasicBlock notBothInt = m_out.newBlock();
    LBasicBlock slowCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 3> results;

    results.append(m_out.anchor(m_out.equal(a, b)));
    m_out.branch(m_out.bitAnd(isInt32(a), isInt32(b)), unsure(continuation), unsure(notBothInt));

    m_out.appendTo(notBothInt, slowCase);
    results.append(m_out.anchor(m_out.booleanTrue));
    m_out.branch(m_out.bitAnd(m_out.equal(a, b), isNotNumber(a)), unsure(continuation), unsure(slowCase));

    m_out.appendTo(slowCase, continuation);
    results.append(m_out.anchor(m_out.notZero64(vmCall(node, Int64, strict ? Entry::operationAOTCompareStrictEq : Entry::operationAOTCompareEq, m_instance, a, b))));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    return m_out.phi(Int32, results);
}

bool Lowering::tryLowerArith(Node* node)
{
    switch (node->opcode) {
#define AOT_BINARY(Struct, opcodeName, method) \
    case opcodeName: { \
        auto bytecode = node->as<Struct>(); \
        method(node, bytecode.m_lhs, bytecode.m_rhs); \
        return true; \
    }
    AOT_BINARY(OpAdd, op_add, lowerBinaryArith)
    AOT_BINARY(OpSub, op_sub, lowerBinaryArith)
    AOT_BINARY(OpMul, op_mul, lowerBinaryArith)
    AOT_BINARY(OpDiv, op_div, lowerBinaryArith)
    AOT_BINARY(OpMod, op_mod, lowerBinaryArith)
    AOT_BINARY(OpPow, op_pow, lowerBinaryArith)
    AOT_BINARY(OpBitand, op_bitand, lowerBitOp)
    AOT_BINARY(OpBitor, op_bitor, lowerBitOp)
    AOT_BINARY(OpBitxor, op_bitxor, lowerBitOp)
    AOT_BINARY(OpLshift, op_lshift, lowerBitOp)
    AOT_BINARY(OpRshift, op_rshift, lowerBitOp)
    AOT_BINARY(OpUrshift, op_urshift, lowerBitOp)
#undef AOT_BINARY

#define AOT_COMPARE(Struct, opcodeName) \
    case opcodeName: { \
        auto bytecode = node->as<Struct>(); \
        setBoolean(node, lowerCompare(node, opcodeName, bytecode.m_lhs, bytecode.m_rhs)); \
        return true; \
    }
    AOT_COMPARE(OpLess, op_less)
    AOT_COMPARE(OpLesseq, op_lesseq)
    AOT_COMPARE(OpGreater, op_greater)
    AOT_COMPARE(OpGreatereq, op_greatereq)
    AOT_COMPARE(OpBelow, op_below)
    AOT_COMPARE(OpBeloweq, op_beloweq)
#undef AOT_COMPARE

    case op_eq: {
        auto bytecode = node->as<OpEq>();
        setBoolean(node, lowerEquality(node, false, bytecode.m_lhs, bytecode.m_rhs));
        return true;
    }
    case op_neq: {
        auto bytecode = node->as<OpNeq>();
        setBoolean(node, m_out.logicalNot(lowerEquality(node, false, bytecode.m_lhs, bytecode.m_rhs)));
        return true;
    }
    case op_stricteq: {
        auto bytecode = node->as<OpStricteq>();
        setBoolean(node, lowerEquality(node, true, bytecode.m_lhs, bytecode.m_rhs));
        return true;
    }
    case op_nstricteq: {
        auto bytecode = node->as<OpNstricteq>();
        setBoolean(node, m_out.logicalNot(lowerEquality(node, true, bytecode.m_lhs, bytecode.m_rhs)));
        return true;
    }
    case op_inc:
        lowerUnaryArith(node, node->as<OpInc>().m_srcDst);
        return true;
    case op_dec:
        lowerUnaryArith(node, node->as<OpDec>().m_srcDst);
        return true;
    case op_negate:
        lowerUnaryArith(node, node->as<OpNegate>().m_operand);
        return true;
    case op_bitnot:
        lowerUnaryArith(node, node->as<OpBitnot>().m_operand);
        return true;
    case op_to_number:
        lowerUnaryArith(node, node->as<OpToNumber>().m_operand);
        return true;
    case op_to_numeric:
        lowerUnaryArith(node, node->as<OpToNumeric>().m_operand);
        return true;
    case op_to_string:
        lowerUnaryArith(node, node->as<OpToString>().m_operand);
        return true;
    case op_unsigned: {
        Node* operand = node->use(node->as<OpUnsigned>().m_operand);
        LValue value = operand->rep() == Rep::Int32 ? lowInt32(operand) : unboxInt32(lowJSValue(operand));
        if (node->isInteger())
            setInt64(node, m_out.zeroExt(value, Int64));
        else
            setDouble(node, m_out.unsignedToDouble(value));
        return true;
    }
    default:
        return false;
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT) && (CPU(ARM64) || CPU(X86_64))
