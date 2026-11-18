/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCodeGenerator.h"
#include "AOTCodeRecords.h"
#include "AOTInstance.h"
#include "AOTNumbering.h"
#include "AOTOperand.h"
#include "JSCJSValue.h"
#include "JSString.h"
#include "JSType.h"
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

class PrimitiveEmitter {
    WTF_MAKE_NONCOPYABLE(PrimitiveEmitter);
protected:
    explicit PrimitiveEmitter(CodeGenerator& generator)
        : m_proc(generator.procedure())
        , m_heaps(generator.heaps())
        , m_out(generator.out())
        , m_mentions(generator.records().mentions)
        , m_instance(generator.pinned().instance)
        , m_instancePrefix(generator.pinned().instancePrefix)
        , m_numberTag(generator.pinned().numberTag)
        , m_notCellMask(generator.pinned().notCellMask)
        , m_cellCageBase(generator.pinned().cellCageBase)
        , m_wordsOfInt32Elements(generator.pinned().wordsOfInt32Elements)
        , m_narrowWordsOfIntegers(generator.pinned().narrowWordsOfIntegers)
        , m_owesFenceBehindStores(generator.owesFenceBehindStores())
    {
    }

    LBasicBlock newBlock() { return m_out.newBlock(); }

    template<typename Usual, typename Rare> void ifElse(Condition, const Usual&, const Rare&);
    template<typename T, typename Usual, typename Rare> T either(Condition, const Usual&, const Rare&);
    template<typename Usual, typename Rare> Operand eitherOperand(Condition, Representation, const Usual&, const Rare&);
    template<typename T> T selectByBranch(Condition, NOESCAPE const Invocable<T()> auto& whenTrue, NOESCAPE const Invocable<T()> auto& whenFalse, Machine::EdgeClass edgeClassWhenTrue = Machine::EdgeClass::Normal);
    Condition asBoolean(NOESCAPE const Invocable<void(BranchTargets)> auto& branch);
    void branchUnless(Condition condition, LBasicBlock rareTarget);
    void branch(Condition condition, BranchTargets targets) { m_out.branch(condition.value(), targets); }

    Condition boolean(bool value) { return Condition(m_out.constInt32(value)); }
    Word32 word32(int32_t value) { return Word32(m_out.constInt32(value)); }
    Integer integer(int64_t value) { return Integer(m_out.constInt64(value)); }
    Number number(double value) { return Number(m_out.constDouble(value)); }
    Pointer pointerConstant(int64_t value) { return Pointer(m_out.constInt64(value)); }
    Boxed boxedConstant(JSValue value) { return Boxed(m_out.constInt64(JSValue::encode(value))); }
    Boxed undefined() { return boxedConstant(jsUndefined()); }

    template<typename Result, typename T> Result unary(LValue (Output::*operation)(LValue), T a) { return Result((m_out.*operation)(a.value())); }
    template<typename Result, typename T> Result binary(LValue (Output::*operation)(LValue, LValue), T a, T b) { return Result((m_out.*operation)(a.value(), b.value())); }

    Integer add(Integer a, Integer b) { return binary<Integer>(&Output::add, a, b); }
    Integer sub(Integer a, Integer b) { return binary<Integer>(&Output::sub, a, b); }
    Integer mul(Integer a, Integer b) { return binary<Integer>(&Output::mul, a, b); }
    Integer neg(Integer a) { return unary<Integer>(&Output::neg, a); }
    Integer bitAnd(Integer a, Integer b) { return binary<Integer>(&Output::bitAnd, a, b); }
    Integer bitOr(Integer a, Integer b) { return binary<Integer>(&Output::bitOr, a, b); }
    Integer bitXor(Integer a, Integer b) { return binary<Integer>(&Output::bitXor, a, b); }
    Condition lessThan(Integer a, Integer b) { return binary<Condition>(&Output::lessThan, a, b); }
    Condition lessThanOrEqual(Integer a, Integer b) { return binary<Condition>(&Output::lessThanOrEqual, a, b); }
    Condition greaterThan(Integer a, Integer b) { return binary<Condition>(&Output::greaterThan, a, b); }
    Condition greaterThanOrEqual(Integer a, Integer b) { return binary<Condition>(&Output::greaterThanOrEqual, a, b); }
    Condition isZero(Integer a) { return unary<Condition>(&Output::isZero64, a); }
    Condition isNotZero(Integer a) { return unary<Condition>(&Output::notZero64, a); }

    Word32 add(Word32 a, Word32 b) { return binary<Word32>(&Output::add, a, b); }
    Word32 sub(Word32 a, Word32 b) { return binary<Word32>(&Output::sub, a, b); }
    Word32 mul(Word32 a, Word32 b) { return binary<Word32>(&Output::mul, a, b); }
    Word32 neg(Word32 a) { return unary<Word32>(&Output::neg, a); }
    Word32 bitAnd(Word32 a, Word32 b) { return binary<Word32>(&Output::bitAnd, a, b); }
    Word32 bitOr(Word32 a, Word32 b) { return binary<Word32>(&Output::bitOr, a, b); }
    Word32 bitXor(Word32 a, Word32 b) { return binary<Word32>(&Output::bitXor, a, b); }
    Word32 shiftLeft(Word32 a, Word32 b) { return binary<Word32>(&Output::shl, a, b); }
    Word32 shiftRight(Word32 a, Word32 b) { return binary<Word32>(&Output::aShr, a, b); }
    Word32 shiftRightUnsigned(Word32 a, Word32 b) { return binary<Word32>(&Output::lShr, a, b); }
    Word32 countLeadingZeros(Word32 a) { return unary<Word32>(&Output::ctlz32, a); }
    Condition lessThan(Word32 a, Word32 b) { return binary<Condition>(&Output::lessThan, a, b); }
    Condition lessThanOrEqual(Word32 a, Word32 b) { return binary<Condition>(&Output::lessThanOrEqual, a, b); }
    Condition greaterThan(Word32 a, Word32 b) { return binary<Condition>(&Output::greaterThan, a, b); }
    Condition greaterThanOrEqual(Word32 a, Word32 b) { return binary<Condition>(&Output::greaterThanOrEqual, a, b); }
    Condition isZero(Word32 a) { return unary<Condition>(&Output::isZero32, a); }
    Condition isNotZero(Word32 a) { return unary<Condition>(&Output::notZero32, a); }
    Condition hasBits(Word32 a, uint32_t mask) { return binary<Condition>(&Output::testNonZero32, a, word32(mask)); }
    Condition hasBits(Integer a, int64_t mask) { return binary<Condition>(&Output::testNonZero64, a, integer(mask)); }
    Condition hasNoBits(Word32 a, uint32_t mask) { return binary<Condition>(&Output::testIsZero32, a, word32(mask)); }

    Number add(Number a, Number b) { return binary<Number>(&Output::doubleAdd, a, b); }
    Number sub(Number a, Number b) { return binary<Number>(&Output::doubleSub, a, b); }
    Number mul(Number a, Number b) { return binary<Number>(&Output::doubleMul, a, b); }
    Number div(Number a, Number b) { return binary<Number>(&Output::doubleDiv, a, b); }
    Number min(Number a, Number b) { return binary<Number>(&Output::doubleMin, a, b); }
    Number max(Number a, Number b) { return binary<Number>(&Output::doubleMax, a, b); }
    Number neg(Number a) { return unary<Number>(&Output::doubleNeg, a); }
    Number abs(Number a) { return unary<Number>(&Output::doubleAbs, a); }
    Number floor(Number a) { return unary<Number>(&Output::doubleFloor, a); }
    Number ceil(Number a) { return unary<Number>(&Output::doubleCeil, a); }
    Number trunc(Number a) { return unary<Number>(&Output::doubleTrunc, a); }
    Number sqrt(Number a) { return unary<Number>(&Output::doubleSqrt, a); }
    Condition equal(Number a, Number b) { return binary<Condition>(&Output::doubleEqual, a, b); }
    Condition notEqualOrUnordered(Number a, Number b) { return binary<Condition>(&Output::doubleNotEqualOrUnordered, a, b); }
    Condition equalOrUnordered(Number a, Number b) { return binary<Condition>(&Output::doubleEqualOrUnordered, a, b); }
    Condition isNeitherZeroNorNaN(Number a) { return isX86_64() ? binary<Condition>(&Output::doubleNotEqualAndOrdered, a, number(0)) : greaterThan(abs(a), 0); }
    Condition lessThan(Number a, Number b) { return binary<Condition>(&Output::doubleLessThan, a, b); }
    Condition lessThanOrEqual(Number a, Number b) { return binary<Condition>(&Output::doubleLessThanOrEqual, a, b); }
    Condition greaterThan(Number a, Number b) { return binary<Condition>(&Output::doubleGreaterThan, a, b); }
    Condition greaterThanOrEqual(Number a, Number b) { return binary<Condition>(&Output::doubleGreaterThanOrEqual, a, b); }
    Number toNumber(Word32 a) { return unary<Number>(&Output::intToDouble, a); }
    Number toNumber(Integer a) { return unary<Number>(&Output::intToDouble, a); }

    template<typename T> static constexpr bool isBitwiseComparable = !std::is_same_v<T, Number> && !std::is_same_v<T, OptionalNumber>;
    template<typename T> requires isBitwiseComparable<T> Condition equal(T a, T b) { return binary<Condition>(&Output::equal, a, b); }
    template<typename T> requires isBitwiseComparable<T> Condition notEqual(T a, T b) { return binary<Condition>(&Output::notEqual, a, b); }
    template<typename T> requires isBitwiseComparable<T> Condition below(T a, T b) { return binary<Condition>(&Output::below, a, b); }
    template<typename T> requires isBitwiseComparable<T> Condition belowOrEqual(T a, T b) { return binary<Condition>(&Output::belowOrEqual, a, b); }
    template<typename T> requires isBitwiseComparable<T> Condition above(T a, T b) { return binary<Condition>(&Output::above, a, b); }
    template<typename T> requires isBitwiseComparable<T> Condition aboveOrEqual(T a, T b) { return binary<Condition>(&Output::aboveOrEqual, a, b); }

    template<typename T> T constant(Immediate<T>);
    template<typename T> T add(T a, Immediate<T> b) { return add(a, constant<T>(b)); }
    template<typename T> T sub(T a, Immediate<T> b) { return sub(a, constant<T>(b)); }
    template<typename T> T mul(T a, Immediate<T> b) { return mul(a, constant<T>(b)); }
    template<typename T> T bitAnd(T a, Immediate<T> b) { return bitAnd(a, constant<T>(b)); }
    template<typename T> T bitOr(T a, Immediate<T> b) { return bitOr(a, constant<T>(b)); }
    template<typename T> T bitXor(T a, Immediate<T> b) { return bitXor(a, constant<T>(b)); }
    Number div(Number a, double b) { return div(a, number(b)); }
    Word32 shiftLeft(Word32 a, int32_t b) { return shiftLeft(a, word32(b)); }
    Word32 shiftRight(Word32 a, int32_t b) { return shiftRight(a, word32(b)); }
    Word32 shiftRightUnsigned(Word32 a, int32_t b) { return shiftRightUnsigned(a, word32(b)); }
    template<typename T> Condition lessThan(T a, Immediate<T> b) { return lessThan(a, constant<T>(b)); }
    template<typename T> Condition lessThanOrEqual(T a, Immediate<T> b) { return lessThanOrEqual(a, constant<T>(b)); }
    template<typename T> Condition greaterThan(T a, Immediate<T> b) { return greaterThan(a, constant<T>(b)); }
    template<typename T> Condition greaterThanOrEqual(T a, Immediate<T> b) { return greaterThanOrEqual(a, constant<T>(b)); }
    template<typename T> Condition equal(T a, Immediate<T> b) { return equal(a, constant<T>(b)); }
    template<typename T> Condition notEqual(T a, Immediate<T> b) { return notEqual(a, constant<T>(b)); }
    template<typename T> Condition below(T a, Immediate<T> b) { return below(a, constant<T>(b)); }
    template<typename T> Condition belowOrEqual(T a, Immediate<T> b) { return belowOrEqual(a, constant<T>(b)); }
    template<typename T> Condition above(T a, Immediate<T> b) { return above(a, constant<T>(b)); }
    template<typename T> Condition aboveOrEqual(T a, Immediate<T> b) { return aboveOrEqual(a, constant<T>(b)); }

    Condition logicalNot(Condition value) { return Condition(m_out.bitXor(value.value(), 1)); }
    Condition logicalAnd(Condition a, Condition b) { return binary<Condition>(&Output::bitAnd, a, b); }
    Condition logicalOr(Condition a, Condition b) { return a ? binary<Condition>(&Output::bitOr, a, b) : b; }
    template<typename T> T select(Condition condition, T a, T b) { return T(m_out.select(condition.value(), a.value(), b.value())); }
    template<typename T> T select(Condition condition, T a, Immediate<T> b) { return select(condition, a, constant<T>(b)); }
    template<typename T> T select(Condition condition, Immediate<T> a, T b) { return select(condition, constant<T>(a), b); }
    template<typename T> T select(Condition, Immediate<T>, Immediate<T>);

    Integer signExtended(Word32 value) { return Integer(m_out.signExt32To64(value.value())); }
    Integer zeroExtended(Word32 value) { return Integer(m_out.zeroExt(value.value(), Machine::Int64)); }
    Integer zeroExtended(Condition value) { return Integer(m_out.zeroExt(value.value(), Machine::Int64)); }
    Word32 low32(Integer value) { return Word32(m_out.castToInt32(value.value())); }
    Word32 low32InWords(Integer value) { return Word32(m_out.lowWord(value.value())); }
    Word32 asWord32(Condition value) { return Word32(value.value()); }
    LValue countTrailingZerosOfNonZero(LValue bits);
    LValue reverseBytes(LValue word);

    Condition isInt32(Boxed value) { return Condition(m_out.aboveOrEqual(value.value(), m_numberTag)); }
    Condition isNotInt32(Boxed value) { return Condition(m_out.below(value.value(), m_numberTag)); }
    Condition isNumber(Boxed value) { return Condition(m_out.testNonZero64(value.value(), m_numberTag)); }
    Condition isCell(Boxed value) { return Condition(m_out.testIsZero64(value.value(), m_notCellMask)); }
    Condition isNotCell(Boxed value) { return Condition(m_out.testNonZero64(value.value(), m_notCellMask)); }
    Condition isCellAmongNonNumbers(Boxed value)
    {
        static_assert(JSValue::ValueEmpty < JSValue::ValueUndefined && JSValue::ValueNull < JSValue::ValueUndefined && JSValue::ValueFalse < JSValue::ValueUndefined && JSValue::ValueTrue < JSValue::ValueUndefined && JSValue::ValueUndefined < sizeof(JSCell) * 2);
        return Condition(m_out.above(value.value(), m_out.constInt64(JSValue::ValueUndefined)));
    }
    static bool areNonNumbers(uint16_t primitives) { return !(primitives & ~(cellPrimitives | Primitive::UndefinedOrNull | Primitive::Boolean | Primitive::Absent)); }
    Condition isCellAmong(Boxed value, uint16_t primitives) { return areNonNumbers(primitives) ? isCellAmongNonNumbers(value) : isCell(value); }
    Condition isCellAndNotEmpty(Boxed value)
    {
        static_assert(JSValue::ValueUndefined < 16 && JSValue::ValueNull < 16 && JSValue::ValueTrue < 16 && JSValue::ValueFalse < 16 && JSValue::ValueDeleted < 16);
#if USE(BIGINT32)
#error "A BigInt32 has no bit from 2^48 up: it would pass for a cell."
#endif
        return Condition(m_out.isZero64(m_out.lShr(m_out.sub(value.value(), m_out.constInt64(16)), 48)));
    }
    Condition isBoolean(Boxed value)
    {
        static_assert(JSValue::ValueTrue == JSValue::ValueFalse + 1);
        return Condition(m_out.belowOrEqual(m_out.sub(value.value(), JSValue::ValueFalse), 1));
    }
    Condition isUndefinedOrNull(Boxed, uint16_t occurringPrimitives);
    Condition isEmpty(Boxed value) { return Condition(m_out.isZero64(value.value())); }
    Condition isNotEmpty(Boxed value) { return Condition(m_out.notZero64(value.value())); }
    Condition isCellReference(NarrowReference value) { return Condition(m_out.above(value.value(), undefinedCell32)); }
    Condition isCellReferenceAmongAllWords(NarrowReference value) { return Condition(m_out.aboveOrEqual(value.value(), minCell32)); }
    std::optional<Word32> narrowWordBehind(OptionalInteger);
    void noteNarrowWord(OptionalInteger widened, Word32 word) { m_narrowWordsOfIntegers.set(widened.value(), word.value()); }

    Boxed boxInt32(Word32 value)
    {
        if (auto word = m_wordsOfInt32Elements.find(value.value()); word != m_wordsOfInt32Elements.end())
            return Boxed(word->value);
        return Boxed(m_out.add(m_out.zeroExt(value.value(), Machine::Int64), m_numberTag));
    }
    Boxed boxBoolean(Condition value)
    {
        static_assert(JSValue::ValueTrue == (JSValue::ValueFalse | 1));
        return Boxed(m_out.bitOr(m_out.zeroExt(value.value(), Machine::Int64), JSValue::ValueFalse));
    }
    Boxed boxCell(CellPointer value) { return Boxed(value.value()); }
    Word32 unboxInt32(Boxed value) { return Word32(m_out.castToInt32(value.value())); }
    Word32 unboxInt32Element(Boxed word)
    {
        Word32 element = unboxInt32(word);
        m_wordsOfInt32Elements.add(element.value(), word.value());
        return element;
    }
    Number unboxDouble(Boxed value) { return Number(m_out.bitCast(m_out.add(value.value(), m_numberTag), Machine::Double)); }
    Condition unboxBoolean(Boxed value) { return Condition(m_out.bitAnd(m_out.castToInt32(value.value()), 1)); }
    CellPointer unboxCell(Boxed value) { return CellPointer(value.value()); }
    Number numberToDouble(Boxed number);

    CellPointer decode(NarrowReference);
    NarrowReference compress(CellPointer);

    Condition hasCellTypeIn(CellPointer, JSTypeRange stableRange);
    Condition hasCellType(CellPointer cell, JSType type) { return hasCellTypeIn(cell, { type, type }); }
    Condition hasCellTypeAbove(CellPointer, JSType);
    Condition hasOrHadCellType(CellPointer, JSType);
    Condition hasMutableCellType(CellPointer, JSType);
    Condition isObjectCell(CellPointer cell) { return hasCellTypeIn(cell, { ObjectType, MaxJSType }); }
    Word32 loadStructureID(CellPointer cell) { return Word32(m_out.load32(cell.value(), m_heaps.JSCell_structureID)); }
    Pointer loadStructure(CellPointer);
    Pointer decodeStructureID(Word32 structureID);

    template<typename T, typename Base> T loadField(Base, const Machine::AbstractHeap&);
    template<typename Base> Word32 loadByte(Base base, const Machine::AbstractHeap& field) { return Word32(m_out.load8ZeroExt32(base.value(), field)); }
    template<typename T, typename Base> void storeField(Base, const Machine::AbstractHeap&, T);
    Condition hasNoBits(Pointer a, uintptr_t mask) { return Condition(m_out.testIsZeroPtr(a.value(), mask)); }
    Pointer offsetBy(Pointer base, Integer index, unsigned logSize) { return Pointer(m_out.add(base.value(), logSize ? m_out.shl(index.value(), logSize) : index.value())); }
    Pointer offsetBy(Pointer base, ptrdiff_t offset) { return offset ? Pointer(m_out.add(base.value(), offset)) : base; }
    Pointer asPointer(CellPointer cell) { return Pointer(cell.value()); }

    Pointer vm() { return Pointer(m_out.loadPtr(m_instance, m_heaps.AOTInstance_vm)); }
    Pointer loadImmutablePointer(ptrdiff_t offsetInInstance);
    Word32 loadImmutable32(ptrdiff_t offsetInInstance);
    Integer loadImmutable64(ptrdiff_t offsetInInstance);
    Word32 loadFilledOnce32(ptrdiff_t offsetInInstance);
    Word32 loadMutable32(ptrdiff_t offsetInInstance);
    TypedPointer instanceField(const Machine::AbstractHeap&, ptrdiff_t offsetInInstance);
    TypedPointer mutableInstanceField(ptrdiff_t offsetInInstance) { return instanceField(m_heaps.AOTInstance_mutableFields, offsetInInstance); }

    Word32 numberOf(uint32_t mention) { return Word32(m_out.number(mention)); }
    TypedPointer instanceWord(const Machine::AbstractHeap& heap, uint32_t mention) { return m_out.instanceWord(heap, mention); }

    Condition isRopeString(CellPointer string) { return Condition(m_out.testNonZeroPtr(m_out.loadPtr(string.value(), m_heaps.JSString_value), JSString::isRopeInPointer)); }
    CellPointer emptyString() { return CellPointer(loadImmutablePointer(AOT_INSTANCE_PREFIX_OFFSET(emptyString)).value()); }
    CellPointer singleCharacterString(Word32 character);

    void mutatorFence();

    void ensureStillAliveHere(CellPointer);

    Machine::Procedure& m_proc;
    const AOTHeaps& m_heaps;
    Output& m_out;
    MentionList& m_mentions;
    LValue& m_instance;
    LValue& m_instancePrefix;
    LValue& m_numberTag;
    LValue& m_notCellMask;
    LValue& m_cellCageBase;
    UncheckedKeyHashMap<LValue, LValue>& m_wordsOfInt32Elements;
    UncheckedKeyHashMap<LValue, LValue>& m_narrowWordsOfIntegers;
    bool& m_owesFenceBehindStores;

private:
    Word32 loadCellType(CellPointer cell) { return Word32(m_out.load8ZeroExt32(cell.value(), m_heaps.JSCell_typeInfoType)); }
};

bool isStableCellTypeRange(JSTypeRange);
void definePinnedValues(Output&, PinnedValues&);

inline void PrimitiveEmitter::branchUnless(Condition condition, LBasicBlock rareTarget)
{
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(condition.value(), normalEdge(continuation), rareEdge(rareTarget));
    m_out.appendToContinuation(continuation);
}

Condition PrimitiveEmitter::asBoolean(NOESCAPE const Invocable<void(BranchTargets)> auto& branch)
{
    LBasicBlock taken = m_out.newBlock();
    LBasicBlock notTaken = m_out.newBlock();
    Join<Condition> join(m_out);
    branch(BranchTargets { normalEdge(taken), normalEdge(notTaken) });
    m_out.appendTo(taken);
    join.add(boolean(true));
    m_out.appendTo(notTaken);
    join.add(boolean(false));
    return join.finish();
}

template<typename T>
T PrimitiveEmitter::selectByBranch(Condition condition, NOESCAPE const Invocable<T()> auto& whenTrue, NOESCAPE const Invocable<T()> auto& whenFalse, Machine::EdgeClass edgeClassWhenTrue)
{
    if (condition.value()->hasInt())
        return condition.value()->asInt() ? whenTrue() : whenFalse();

    LBasicBlock isTrue = m_out.newBlock();
    LBasicBlock isFalse = m_out.newBlock();
    Join<T> join(m_out);
    ScopeFork fork = m_out.forkScope();
    m_out.branch(condition.value(), BlockEdge(isTrue, edgeClassWhenTrue), normalEdge(isFalse));
    m_out.appendToArm(isTrue, fork);
    join.add(whenTrue());
    m_out.appendToArm(isFalse, fork);
    join.add(whenFalse());
    return join.finishBehind(fork);
}

template<typename T>
T PrimitiveEmitter::constant(Immediate<T> value)
{
    if constexpr (std::is_same_v<T, Word32>)
        return word32(value);
    else if constexpr (std::is_same_v<T, Integer>)
        return integer(value);
    else
        return number(value);
}

template<typename T>
T PrimitiveEmitter::select(Condition condition, Immediate<T> a, Immediate<T> b)
{
    T whenTrue = constant<T>(a);
    return select(condition, whenTrue, constant<T>(b));
}

template<typename T, typename Base>
T PrimitiveEmitter::loadField(Base base, const Machine::AbstractHeap& field)
{
    return T(m_out.load(m_out.address(base.value(), field), machineTypeOf(T::representation)));
}

template<typename T, typename Base>
void PrimitiveEmitter::storeField(Base base, const Machine::AbstractHeap& field, T value)
{
    m_out.store(value.value(), m_out.address(base.value(), field));
}

template<typename Usual, typename Rare>
void PrimitiveEmitter::ifElse(Condition condition, const Usual& usual, const Rare& rare)
{
    LBasicBlock yes = m_out.newBlock();
    LBasicBlock no = m_out.newBlock();
    LBasicBlock join = m_out.newBlock();
    ScopeFork fork = m_out.forkScope();
    m_out.branch(condition.value(), normalEdge(yes), rareEdge(no));
    m_out.appendToArm(yes, fork);
    usual();
    m_out.jump(join);
    m_out.appendToArm(no, fork);
    rare();
    m_out.jump(join);
    m_out.appendToJoin(join, fork);
}

template<typename Usual, typename Rare>
Operand PrimitiveEmitter::eitherOperand(Condition condition, Representation representation, const Usual& usual, const Rare& rare)
{
    LBasicBlock yes = m_out.newBlock();
    LBasicBlock no = m_out.newBlock();
    OperandJoin join(m_out, representation);
    ScopeFork fork = m_out.forkScope();
    m_out.branch(condition.value(), normalEdge(yes), rareEdge(no));
    m_out.appendToArm(yes, fork);
    join.add(usual());
    m_out.appendToArm(no, fork);
    join.add(rare());
    return join.finishBehind(fork);
}

template<typename T, typename Usual, typename Rare>
T PrimitiveEmitter::either(Condition condition, const Usual& usual, const Rare& rare)
{
    static_assert(std::is_same_v<decltype(usual()), T> && std::is_same_v<decltype(rare()), T>);
    return eitherOperand(condition, T::representation, [&]() -> Operand {
        return usual();
    }, [&]() -> Operand {
        return rare();
    }).template as<T>();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
