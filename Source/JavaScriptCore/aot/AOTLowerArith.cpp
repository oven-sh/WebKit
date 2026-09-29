/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLowering.h"

#if ENABLE(FTL_JIT)

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
    return callStub(stub, type, { { a, GPRInfo::argumentGPR0 }, { b, GPRInfo::argumentGPR1 } }, { }, StubClobbers::WhatCallsDo, node);
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
        // It has been proven that the result is what it would be in doubles. In 64 bits nothing is lost on the way to it either.
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
            // Of something that is not negative by something that is positive.
            result = m_out.sub(a, m_out.mul(m_out.div(a, b), b));
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
        setResult(node, result, narrow ? Rep::Int32 : Rep::Int64);
        return;
    }
    if (isSubtype(left->type | right->type, TNumber)) {
        setDouble(node, doubleOp(lowDouble(left), lowDouble(right)));
        return;
    }

    // Nothing is known. Numbers are handled here; everything else is the runtime's.
    LValue a = lowJSValue(left);
    LValue b = lowJSValue(right);
    bool mayBeNumbers = mayBe(left->type, TNumber) && mayBe(right->type, TNumber);
    if (!mayBeNumbers) {
        setJSValue(node, vmCall(node, Int64, operationFor(opcode), m_globalObject, a, b));
        return;
    }
    if (isCompact()) {
        if (auto stub = stubFor(opcode))
            setJSValue(node, callBinaryStub(node, *stub, Int64, a, b));
        else
            setJSValue(node, vmCall(node, Int64, operationFor(opcode), m_globalObject, a, b));
        return;
    }

    LBasicBlock intCase = m_out.newBlock();
    LBasicBlock notBothInt = m_out.newBlock();
    LBasicBlock doubleCase = m_out.newBlock();
    LBasicBlock slowCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 3> results;

    bool hasIntCase = opcode == op_add || opcode == op_sub || opcode == op_mul;
    if (hasIntCase)
        m_out.branch(m_out.bitAnd(isInt32(a), isInt32(b)), unsure(intCase), unsure(notBothInt));
    else
        m_out.jump(notBothInt);

    m_out.appendTo(intCase, notBothInt);
    if (hasIntCase) {
        // In 64 bits none of these overflow, so whether the result is an int32 can be asked afterwards.
        LValue wideA = m_out.signExt32To64(unboxInt32(a));
        LValue wideB = m_out.signExt32To64(unboxInt32(b));
        LValue wide = opcode == op_add ? m_out.add(wideA, wideB) : opcode == op_sub ? m_out.sub(wideA, wideB) : m_out.mul(wideA, wideB);
        LValue narrow = m_out.castToInt32(wide);
        LValue fits = m_out.equal(m_out.signExt32To64(narrow), wide);
        if (opcode == op_mul) {
            // 0 * -1 is -0.
            fits = m_out.bitAnd(fits, m_out.bitOr(m_out.notZero32(narrow), m_out.greaterThanOrEqual(m_out.bitOr(wideA, wideB), m_out.constInt64(0))));
        }
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
    results.append(m_out.anchor(vmCall(node, Int64, operationFor(opcode), m_globalObject, a, b)));
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
    // Sometimes one and sometimes another, so it is boxed. All three are computed, and one is picked.
    RELEASE_ASSERT(isSubtype(operand->type, TNumber | TBoolean));
    LValue value = lowRaw(operand);
    LValue result = unboxBoolean(value);
    if (mayBe(operand->type, TDouble))
        result = m_out.select(isNumber(value), doubleToInt32(unboxDouble(value)), result);
    if (mayBe(operand->type, TInt32))
        result = m_out.select(isInt32(value), unboxInt32(value), result);
    return result;
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
    if (isCompact()) {
        setJSValue(node, callBinaryStub(node, *stubFor(opcode), Int64, a, b));
        return;
    }
    LBasicBlock intCase = m_out.newBlock();
    LBasicBlock slowCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(m_out.bitAnd(isInt32(a), isInt32(b)), usually(intCase), rarely(slowCase));

    m_out.appendTo(intCase, slowCase);
    LValue fast = intOp(unboxInt32(a), unboxInt32(b));
    ValueFromBlock fastResult = m_out.anchor(boxInt32(fast));
    m_out.jump(continuation);

    m_out.appendTo(slowCase, continuation);
    ValueFromBlock slowResult = m_out.anchor(vmCall(node, Int64, operationFor(opcode), m_globalObject, a, b));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    setJSValue(node, m_out.phi(Int64, fastResult, slowResult));
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
        // 2^53 + 1 is 2^53.
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

    LValue value = lowJSValue(operand);
    LBasicBlock fastCase = m_out.newBlock();
    LBasicBlock slowCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 2> results;

    switch (opcode) {
    case op_inc:
    case op_dec: {
        // Away from the ends of the range an int32 stays one.
        int32_t limit = opcode == op_inc ? INT32_MAX : INT32_MIN;
        m_out.branch(m_out.bitAnd(isInt32(value), m_out.notEqual(unboxInt32(value), m_out.constInt32(limit))), usually(fastCase), rarely(slowCase));
        m_out.appendTo(fastCase, slowCase);
        results.append(m_out.anchor(boxInt32(m_out.add(unboxInt32(value), m_out.constInt32(opcode == op_inc ? 1 : -1)))));
        break;
    }
    case op_negate:
        // Not 0 (whose negation is -0) and not INT32_MIN.
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
    results.append(m_out.anchor(vmCall(node, Int64, operationFor(opcode), m_globalObject, value)));
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
        // Only ever emitted for values the bytecode generator knows to be int32s.
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

    LValue a = lowJSValue(left);
    LValue b = lowJSValue(right);
    if (isCompact())
        return callBinaryStub(node, *stubFor(opcode), Int32, a, b);
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
    results.append(m_out.anchor(m_out.notZero64(vmCall(node, Int64, operationFor(opcode), m_globalObject, a, b))));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    return m_out.phi(Int32, results);
}

LValue Lowering::lowerEquality(Node* node, bool strict, VirtualRegister lhs, VirtualRegister rhs)
{
    Node* left = node->use(lhs);
    Node* right = node->use(rhs);
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
        return m_out.booleanFalse; // No value is of both types.

    LValue a = lowJSValue(left);
    LValue b = lowJSValue(right);

    // Where identity of the bits is the whole story: neither side can be a number (int32 1 and double 1 are equal, NaN is not
    // equal to itself), a string or a BigInt (equal by content), and for == neither can be converted.
    Type byContent = TNumber | TString | TBigInt;
    if (strict) {
        if (!mayBe(left->type, byContent) || !mayBe(right->type, byContent))
            return m_out.equal(a, b);
    } else if (isSubtype(both, TAnyObject | TSymbol) || isSubtype(both, TBoolean))
        return m_out.equal(a, b);

    if (isCompact()) {
        return callStub(strict ? Stub::StrictEqual : Stub::LooseEqual, Int32, { { a, GPRInfo::argumentGPR0 }, { b, GPRInfo::argumentGPR1 } },
            { });
    }

    LBasicBlock notBothInt = m_out.newBlock();
    LBasicBlock slowCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 3> results;

    results.append(m_out.anchor(m_out.equal(a, b)));
    m_out.branch(m_out.bitAnd(isInt32(a), isInt32(b)), unsure(continuation), unsure(notBothInt));

    m_out.appendTo(notBothInt, slowCase);
    // The same bits, and not a number: equal, whatever it is.
    results.append(m_out.anchor(m_out.booleanTrue));
    m_out.branch(m_out.bitAnd(m_out.equal(a, b), isNotNumber(a)), unsure(continuation), unsure(slowCase));

    m_out.appendTo(slowCase, continuation);
    results.append(m_out.anchor(m_out.notZero64(vmCall(node, Int64, strict ? Entry::operationAOTCompareStrictEq : Entry::operationAOTCompareEq, m_globalObject, a, b))));
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

#endif // ENABLE(FTL_JIT)
