/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCodeGenerator.h"
#include "AOTInstance.h"
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
        , m_instance(generator.pinned().instance)
        , m_instancePrefix(generator.pinned().instancePrefix)
        , m_statics(generator.pinned().statics)
        , m_numberTag(generator.pinned().numberTag)
        , m_notCellMask(generator.pinned().notCellMask)
        , m_cellCageBase(generator.pinned().cellCageBase)
    {
    }

    LBasicBlock newBlock() { return m_out.newBlock(); }

    template<typename Usual, typename Rare> void ifElse(Condition, const Usual&, const Rare&);
    template<typename T, typename Usual, typename Rare> T either(Condition, const Usual&, const Rare&);
    template<typename Usual, typename Rare> Operand eitherOperand(Condition, Representation, const Usual&, const Rare&);
    // Both sides are as likely as the code before them.
    template<typename T> T selectByBranch(Condition, NOESCAPE const Invocable<T()> auto& whenTrue, NOESCAPE const Invocable<T()> auto& whenFalse);
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

    template<typename Result, typename T> Result unary(LValue (FTL::Output::*operation)(LValue), T a) { return Result((m_out.*operation)(a.value())); }
    template<typename Result, typename T> Result binary(LValue (FTL::Output::*operation)(LValue, LValue), T a, T b) { return Result((m_out.*operation)(a.value(), b.value())); }

    Integer add(Integer a, Integer b) { return binary<Integer>(&FTL::Output::add, a, b); }
    Integer sub(Integer a, Integer b) { return binary<Integer>(&FTL::Output::sub, a, b); }
    Integer mul(Integer a, Integer b) { return binary<Integer>(&FTL::Output::mul, a, b); }
    Integer mod(Integer a, Integer b) { return binary<Integer>(&FTL::Output::mod, a, b); }
    Integer neg(Integer a) { return unary<Integer>(&FTL::Output::neg, a); }
    Integer bitAnd(Integer a, Integer b) { return binary<Integer>(&FTL::Output::bitAnd, a, b); }
    Integer bitOr(Integer a, Integer b) { return binary<Integer>(&FTL::Output::bitOr, a, b); }
    Integer bitXor(Integer a, Integer b) { return binary<Integer>(&FTL::Output::bitXor, a, b); }
    Condition lessThan(Integer a, Integer b) { return binary<Condition>(&FTL::Output::lessThan, a, b); }
    Condition lessThanOrEqual(Integer a, Integer b) { return binary<Condition>(&FTL::Output::lessThanOrEqual, a, b); }
    Condition greaterThan(Integer a, Integer b) { return binary<Condition>(&FTL::Output::greaterThan, a, b); }
    Condition greaterThanOrEqual(Integer a, Integer b) { return binary<Condition>(&FTL::Output::greaterThanOrEqual, a, b); }
    Condition isZero(Integer a) { return unary<Condition>(&FTL::Output::isZero64, a); }
    Condition isNotZero(Integer a) { return unary<Condition>(&FTL::Output::notZero64, a); }

    Word32 add(Word32 a, Word32 b) { return binary<Word32>(&FTL::Output::add, a, b); }
    Word32 sub(Word32 a, Word32 b) { return binary<Word32>(&FTL::Output::sub, a, b); }
    Word32 mul(Word32 a, Word32 b) { return binary<Word32>(&FTL::Output::mul, a, b); }
    Word32 neg(Word32 a) { return unary<Word32>(&FTL::Output::neg, a); }
    Word32 bitAnd(Word32 a, Word32 b) { return binary<Word32>(&FTL::Output::bitAnd, a, b); }
    Word32 bitOr(Word32 a, Word32 b) { return binary<Word32>(&FTL::Output::bitOr, a, b); }
    Word32 bitXor(Word32 a, Word32 b) { return binary<Word32>(&FTL::Output::bitXor, a, b); }
    Word32 shiftLeft(Word32 a, Word32 b) { return binary<Word32>(&FTL::Output::shl, a, b); }
    Word32 shiftRight(Word32 a, Word32 b) { return binary<Word32>(&FTL::Output::aShr, a, b); }
    Word32 shiftRightUnsigned(Word32 a, Word32 b) { return binary<Word32>(&FTL::Output::lShr, a, b); }
    Word32 countLeadingZeros(Word32 a) { return unary<Word32>(&FTL::Output::ctlz32, a); }
    Condition lessThan(Word32 a, Word32 b) { return binary<Condition>(&FTL::Output::lessThan, a, b); }
    Condition lessThanOrEqual(Word32 a, Word32 b) { return binary<Condition>(&FTL::Output::lessThanOrEqual, a, b); }
    Condition greaterThan(Word32 a, Word32 b) { return binary<Condition>(&FTL::Output::greaterThan, a, b); }
    Condition greaterThanOrEqual(Word32 a, Word32 b) { return binary<Condition>(&FTL::Output::greaterThanOrEqual, a, b); }
    Condition isZero(Word32 a) { return unary<Condition>(&FTL::Output::isZero32, a); }
    Condition isNotZero(Word32 a) { return unary<Condition>(&FTL::Output::notZero32, a); }
    Condition hasBits(Word32 a, uint32_t mask) { return binary<Condition>(&FTL::Output::testNonZero32, a, word32(mask)); }
    Condition hasBits(Integer a, int64_t mask) { return binary<Condition>(&FTL::Output::testNonZero64, a, integer(mask)); }
    Condition hasNoBits(Word32 a, uint32_t mask) { return binary<Condition>(&FTL::Output::testIsZero32, a, word32(mask)); }

    Number add(Number a, Number b) { return binary<Number>(&FTL::Output::doubleAdd, a, b); }
    Number sub(Number a, Number b) { return binary<Number>(&FTL::Output::doubleSub, a, b); }
    Number mul(Number a, Number b) { return binary<Number>(&FTL::Output::doubleMul, a, b); }
    Number div(Number a, Number b) { return binary<Number>(&FTL::Output::doubleDiv, a, b); }
    Number min(Number a, Number b) { return binary<Number>(&FTL::Output::doubleMin, a, b); }
    Number max(Number a, Number b) { return binary<Number>(&FTL::Output::doubleMax, a, b); }
    Number neg(Number a) { return unary<Number>(&FTL::Output::doubleNeg, a); }
    Number abs(Number a) { return unary<Number>(&FTL::Output::doubleAbs, a); }
    Number floor(Number a) { return unary<Number>(&FTL::Output::doubleFloor, a); }
    Number ceil(Number a) { return unary<Number>(&FTL::Output::doubleCeil, a); }
    Number trunc(Number a) { return unary<Number>(&FTL::Output::doubleTrunc, a); }
    Number sqrt(Number a) { return unary<Number>(&FTL::Output::doubleSqrt, a); }
    Condition equal(Number a, Number b) { return binary<Condition>(&FTL::Output::doubleEqual, a, b); }
    Condition notEqualOrUnordered(Number a, Number b) { return binary<Condition>(&FTL::Output::doubleNotEqualOrUnordered, a, b); }
    Condition equalOrUnordered(Number a, Number b) { return binary<Condition>(&FTL::Output::doubleEqualOrUnordered, a, b); }
    Condition isNeitherZeroNorNaN(Number a) { return isX86_64() ? binary<Condition>(&FTL::Output::doubleNotEqualAndOrdered, a, number(0)) : greaterThan(abs(a), number(0)); }
    Condition lessThan(Number a, Number b) { return binary<Condition>(&FTL::Output::doubleLessThan, a, b); }
    Condition lessThanOrEqual(Number a, Number b) { return binary<Condition>(&FTL::Output::doubleLessThanOrEqual, a, b); }
    Condition greaterThan(Number a, Number b) { return binary<Condition>(&FTL::Output::doubleGreaterThan, a, b); }
    Condition greaterThanOrEqual(Number a, Number b) { return binary<Condition>(&FTL::Output::doubleGreaterThanOrEqual, a, b); }
    Number toNumber(Word32 a) { return unary<Number>(&FTL::Output::intToDouble, a); }
    Number toNumber(Integer a) { return unary<Number>(&FTL::Output::intToDouble, a); }

    template<typename T> static constexpr bool isBitwiseComparable = !std::is_same_v<T, Number> && !std::is_same_v<T, OptionalNumber>;
    template<typename T> requires isBitwiseComparable<T> Condition equal(T a, T b) { return binary<Condition>(&FTL::Output::equal, a, b); }
    template<typename T> requires isBitwiseComparable<T> Condition notEqual(T a, T b) { return binary<Condition>(&FTL::Output::notEqual, a, b); }
    template<typename T> requires isBitwiseComparable<T> Condition below(T a, T b) { return binary<Condition>(&FTL::Output::below, a, b); }
    template<typename T> requires isBitwiseComparable<T> Condition belowOrEqual(T a, T b) { return binary<Condition>(&FTL::Output::belowOrEqual, a, b); }
    template<typename T> requires isBitwiseComparable<T> Condition above(T a, T b) { return binary<Condition>(&FTL::Output::above, a, b); }
    template<typename T> requires isBitwiseComparable<T> Condition aboveOrEqual(T a, T b) { return binary<Condition>(&FTL::Output::aboveOrEqual, a, b); }

    Condition logicalNot(Condition value) { return Condition(m_out.bitXor(value.value(), m_out.constInt32(1))); }
    Condition logicalAnd(Condition a, Condition b) { return binary<Condition>(&FTL::Output::bitAnd, a, b); }
    Condition logicalOr(Condition a, Condition b) { return a ? binary<Condition>(&FTL::Output::bitOr, a, b) : b; }
    template<typename T> T select(Condition condition, T a, T b) { return T(m_out.select(condition.value(), a.value(), b.value())); }

    Integer signExtended(Word32 value) { return Integer(m_out.signExt32To64(value.value())); }
    Integer zeroExtended(Word32 value) { return Integer(m_out.zeroExt(value.value(), B3::Int64)); }
    Integer zeroExtended(Condition value) { return Integer(m_out.zeroExt(value.value(), B3::Int64)); }
    Word32 low32(Integer value) { return Word32(m_out.castToInt32(value.value())); }
    Word32 low32(Boxed value) { return Word32(m_out.castToInt32(value.value())); }
    Word32 asWord32(Condition value) { return Word32(value.value()); }

    Condition isInt32(Boxed value) { return Condition(m_out.aboveOrEqual(value.value(), m_numberTag)); }
    Condition isNumber(Boxed value) { return Condition(m_out.testNonZero64(value.value(), m_numberTag)); }
    Condition isCell(Boxed value) { return Condition(m_out.testIsZero64(value.value(), m_notCellMask)); }
    Condition isNotCell(Boxed value) { return Condition(m_out.testNonZero64(value.value(), m_notCellMask)); }
    Condition isBoolean(Boxed value)
    {
        static_assert(JSValue::ValueTrue == JSValue::ValueFalse + 1);
        return Condition(m_out.belowOrEqual(m_out.sub(value.value(), m_out.constInt64(JSValue::ValueFalse)), m_out.constInt64(1)));
    }
    Condition isUndefinedOrNull(Boxed, uint16_t occurringPrimitives);
    Condition isEmpty(Boxed value) { return Condition(m_out.isZero64(value.value())); }
    Condition isNotEmpty(Boxed value) { return Condition(m_out.notZero64(value.value())); }
    // A value in a narrow reference is a reference or one of the three markers, and no marker is above undefinedCell32: the comparison takes an immediate of one byte.
    Condition isCellReference(NarrowReference value) { return Condition(m_out.above(value.value(), m_out.constInt32(undefinedCell32))); }
    // For a word that may be any word below minCell32: that of a literal string, an immediate in the table of a collection.
    Condition isCellReferenceAmongAllWords(NarrowReference value) { return Condition(m_out.aboveOrEqual(value.value(), m_out.constInt32(minCell32))); }
    // What a load makes of the word in a narrow slot: the cell where the word is a reference, the word itself where it is none. Every cell lies above every word that is no reference,
    // so a comparison with such a word has one answer on both, and the narrow word needs no decoding.
    std::optional<NarrowReference> narrowWordBehind(LiteralStringWord);

    Boxed boxInt32(Word32 value) { return Boxed(m_out.add(m_out.zeroExt(value.value(), B3::Int64), m_numberTag)); }
    Boxed boxBoolean(Condition value)
    {
        static_assert(JSValue::ValueTrue == (JSValue::ValueFalse | 1));
        return Boxed(m_out.bitOr(m_out.zeroExt(value.value(), B3::Int64), m_out.constInt64(JSValue::ValueFalse)));
    }
    Boxed boxCell(CellPointer value) { return Boxed(value.value()); }
    Word32 unboxInt32(Boxed value) { return Word32(m_out.castToInt32(value.value())); }
    Number unboxDouble(Boxed value) { return Number(m_out.bitCast(m_out.add(value.value(), m_numberTag), B3::Double)); }
    Condition unboxBoolean(Boxed value) { return Condition(m_out.bitAnd(m_out.castToInt32(value.value()), m_out.constInt32(1))); }
    CellPointer unboxCell(Boxed value) { return CellPointer(value.value()); }
    Number numberToDouble(Boxed number);

    CellPointer decode(NarrowReference);
    NarrowReference compress(CellPointer);

    Word32 loadCellType(CellPointer cell) { return Word32(m_out.load8ZeroExt32(cell.value(), m_heaps.JSCell_typeInfoType)); }
    Condition hasCellTypeIn(CellPointer, JSTypeRange stableRange);
    Condition hasCellType(CellPointer cell, JSType type) { return hasCellTypeIn(cell, { type, type }); }
    Condition hasMutableCellType(CellPointer, JSType);
    Condition isObjectCell(CellPointer cell) { return hasCellTypeIn(cell, { ObjectType, MaxJSType }); }
    Word32 loadUsefulBytes(CellPointer cell) { return Word32(m_out.load32(cell.value(), m_heaps.JSCell_usefulBytes)); }
    Word32 loadStructureID(CellPointer cell) { return Word32(m_out.load32(cell.value(), m_heaps.JSCell_structureID)); }
    Pointer loadStructure(CellPointer);
    Pointer decodeStructureID(Word32 structureID);

    template<typename T, typename Base> T loadField(Base, const B3::AbstractHeap&);
    template<typename Base> Word32 loadByte(Base base, const B3::AbstractHeap& field) { return Word32(m_out.load8ZeroExt32(base.value(), field)); }
    template<typename T, typename Base> void storeField(Base, const B3::AbstractHeap&, T);
    Condition hasNoBits(Pointer a, uintptr_t mask) { return Condition(m_out.testIsZeroPtr(a.value(), m_out.constIntPtr(mask))); }
    Pointer offsetBy(Pointer base, Integer index, unsigned logSize) { return Pointer(m_out.add(base.value(), logSize ? m_out.shl(index.value(), m_out.constInt32(logSize)) : index.value())); }
    Pointer offsetBy(Pointer base, ptrdiff_t offset) { return offset ? Pointer(m_out.add(base.value(), m_out.constIntPtr(offset))) : base; }
    Pointer asPointer(CellPointer cell) { return Pointer(cell.value()); }

    Pointer instance() { return Pointer(m_instance); }
    Pointer vm() { return Pointer(m_out.loadPtr(m_instance, m_heaps.AOTInstance_vm)); }
    Pointer loadImmutablePointer(ptrdiff_t offsetInInstance);
    Word32 loadImmutable32(ptrdiff_t offsetInInstance);
    Integer loadImmutable64(ptrdiff_t offsetInInstance);
    Word32 loadMutable32(ptrdiff_t offsetInInstance);
    Integer loadMutable64(ptrdiff_t offsetInInstance);
    TypedPointer instanceField(const B3::AbstractHeap&, ptrdiff_t offsetInInstance);
    TypedPointer mutableInstanceField(ptrdiff_t offsetInInstance) { return instanceField(m_heaps.AOTInstance_mutableFields, offsetInInstance); }

    Condition isRopeString(CellPointer string) { return Condition(m_out.testNonZeroPtr(m_out.loadPtr(string.value(), m_heaps.JSString_value), m_out.constIntPtr(JSString::isRopeInPointer))); }
    CellPointer emptyString() { return CellPointer(loadImmutablePointer(AOT_INSTANCE_PREFIX_OFFSET(emptyString)).value()); }
    CellPointer singleCharacterString(Word32 character);

    void mutatorFence();

    // Ends every read through a pointer loaded from a cell (a view's vector, a string's characters): B3 hoists and shares that load, and then nothing else keeps the cell visible to the conservative scan.
    void ensureStillAliveHere(CellPointer);

    B3::Procedure& m_proc;
    AOTHeaps& m_heaps;
    Output& m_out;
    LValue& m_instance;
    LValue& m_instancePrefix;
    LValue& m_statics;
    LValue& m_numberTag;
    LValue& m_notCellMask;
    LValue& m_cellCageBase;
};

bool isStableCellTypeRange(JSTypeRange);
void pinRegisters(B3::Procedure&);
void definePinnedValues(Output&, B3::Procedure&, InstanceSizes, PinnedValues&);

inline void PrimitiveEmitter::branchUnless(Condition condition, LBasicBlock rareTarget)
{
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(condition.value(), normalEdge(continuation), rareEdge(rareTarget));
    m_out.appendTo(continuation);
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
T PrimitiveEmitter::selectByBranch(Condition condition, NOESCAPE const Invocable<T()> auto& whenTrue, NOESCAPE const Invocable<T()> auto& whenFalse)
{
    if (condition.value()->hasInt())
        return condition.value()->asInt() ? whenTrue() : whenFalse();

    LBasicBlock isTrue = m_out.newBlock();
    LBasicBlock isFalse = m_out.newBlock();
    Join<T> join(m_out);
    m_out.branch(condition.value(), normalEdge(isTrue), normalEdge(isFalse));
    m_out.appendTo(isTrue);
    join.add(whenTrue());
    m_out.appendTo(isFalse);
    join.add(whenFalse());
    return join.finish();
}

template<typename T, typename Base>
T PrimitiveEmitter::loadField(Base base, const B3::AbstractHeap& field)
{
    return T(m_out.load(m_out.address(base.value(), field), b3TypeOf(T::representation)));
}

template<typename T, typename Base>
void PrimitiveEmitter::storeField(Base base, const B3::AbstractHeap& field, T value)
{
    m_out.store(value.value(), m_out.address(base.value(), field));
}

template<typename Usual, typename Rare>
void PrimitiveEmitter::ifElse(Condition condition, const Usual& usual, const Rare& rare)
{
    LBasicBlock yes = m_out.newBlock();
    LBasicBlock no = m_out.newBlock();
    LBasicBlock join = m_out.newBlock();
    m_out.branch(condition.value(), normalEdge(yes), rareEdge(no));
    m_out.appendTo(yes);
    usual();
    m_out.jump(join);
    m_out.appendTo(no);
    rare();
    m_out.jump(join);
    m_out.appendTo(join);
}

template<typename Usual, typename Rare>
Operand PrimitiveEmitter::eitherOperand(Condition condition, Representation representation, const Usual& usual, const Rare& rare)
{
    LBasicBlock yes = m_out.newBlock();
    LBasicBlock no = m_out.newBlock();
    OperandJoin join(m_out, representation);
    m_out.branch(condition.value(), normalEdge(yes), rareEdge(no));
    m_out.appendTo(yes);
    join.add(usual());
    m_out.appendTo(no);
    join.add(rare());
    return join.finish();
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
