/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFormsSaid.h"
#include "AOTImageFormat.h"
#include "AOTMachineBuilder.h"
#include "AOTMachineFrame.h"
#include "AOTMachineTarget.h"
#include "AOTRelocation.h"
#include "JSCJSValue.h"
#include "Reg.h"
#include <span>
#include <type_traits>

namespace JSC { namespace AOT {

using LBasicBlock = Machine::Block*;
using LType = Machine::Type;
using LValue = Machine::Operation*;
using BlockEdge = Machine::Edge;
using SwitchTarget = Machine::SwitchCase;
using ScopeFork = Machine::ScopeFork;

inline BlockEdge normalEdge(LBasicBlock block) { return BlockEdge(block, Machine::EdgeClass::Normal); }
inline BlockEdge unlikelyEdge(LBasicBlock block) { return BlockEdge(block, Machine::EdgeClass::Unlikely); }
inline BlockEdge rareEdge(LBasicBlock block) { return BlockEdge(block, Machine::EdgeClass::Rare); }

struct BranchTargets {
    BlockEdge taken;
    BlockEdge notTaken;

    BranchTargets swapped() const { return { notTaken, taken }; }
};

enum Scale : uint8_t { ScaleOne, ScaleTwo, ScaleFour, ScaleEight, ScalePtr };

constexpr unsigned logScale(Scale scale)
{
    switch (scale) {
    case ScaleOne:
        return 0;
    case ScaleTwo:
        return 1;
    case ScaleFour:
        return 2;
    case ScaleEight:
    case ScalePtr:
        return 3;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

class TypedPointer {
public:
    TypedPointer() = default;
    TypedPointer(const Machine::AbstractHeap& heap, LValue base, ptrdiff_t offset = 0)
        : m_heap(heap)
        , m_base(base)
        , m_offset(offset)
    {
    }

    explicit operator bool() const { return m_heap.isInitialized(); }
    const Machine::AbstractHeap* heap() const LIFETIME_BOUND { return &m_heap; }
    LValue base() const { return m_base; }
    ptrdiff_t offset() const { return m_offset; }

private:
    Machine::AbstractHeap m_heap;
    LValue m_base { nullptr };
    ptrdiff_t m_offset { 0 };
};

class ValueFromBlock {
public:
    ValueFromBlock() = default;
    ValueFromBlock(LValue upsilon, LBasicBlock block)
        : m_upsilon(upsilon)
        , m_block(block)
    {
    }

    explicit operator bool() const { return m_upsilon || m_block; }
    LValue value() const { return m_upsilon; }
    LBasicBlock block() const { return m_block; }

private:
    LValue m_upsilon { nullptr };
    LBasicBlock m_block { nullptr };
};

class Output final : public Machine::Builder {
public:
    explicit Output(Machine::Procedure& procedure)
        : Machine::Builder(procedure)
        , booleanTrue(const32(1))
        , booleanFalse(const32(0))
        , int32Zero(booleanFalse)
        , int32One(booleanTrue)
        , int64Zero(const64(0))
        , intPtrZero(int64Zero)
        , intPtrEight(const64(8))
    {
    }

    const LValue booleanTrue;
    const LValue booleanFalse;
    const LValue int32Zero;
    const LValue int32One;
    const LValue int64Zero;
    const LValue intPtrZero;
    const LValue intPtrEight;

    LValue constInt32(int32_t value) { return const32(value); }
    LValue constInt64(int64_t value) { return const64(value); }
    using Machine::Builder::constDouble;
    template<typename T>
    LValue constIntPtr(T value) { return const64(static_cast<intptr_t>(value)); }

    LValue lockedStackSlot(uint64_t bytes) { return slotBase(m_proc.addStackSlot(safeCast<unsigned>(bytes))); }

    ValueFromBlock anchor(LValue value) { return ValueFromBlock(upsilon(value), m_block); }
    using Machine::Builder::phi;
    template<typename... Params>
    LValue phi(LType type, ValueFromBlock value, Params... theRest)
    {
        LValue result = phi(type);
        addIncomingToPhi(result, value, theRest...);
        return result;
    }
    template<typename VectorType>
    requires requires(const VectorType& vector) { vector.size(); }
    LValue phi(LType type, const VectorType& vector)
    {
        LValue result = phi(type);
        for (const ValueFromBlock& value : vector)
            addIncomingToPhi(result, value);
        return result;
    }
    void addIncomingToPhi(LValue phi, ValueFromBlock value) { setPhi(value.value(), phi); }
    template<typename... Params>
    void addIncomingToPhi(LValue phi, ValueFromBlock value, Params... theRest)
    {
        addIncomingToPhi(phi, value);
        addIncomingToPhi(phi, theRest...);
    }

    LValue add(LValue left, LValue right) { return binary(Machine::Add, left, right); }
    LValue sub(LValue left, LValue right) { return binary(Machine::Sub, left, right); }
    LValue mul(LValue left, LValue right) { return binary(Machine::Mul, left, right); }
    LValue mulHigh(LValue left, LValue right) { return binary(Machine::MulHigh, left, right); }
    LValue uMulHigh(LValue left, LValue right) { return binary(Machine::UMulHigh, left, right); }
    LValue div(LValue left, LValue right) { return binary(Machine::Div, left, right); }
    LValue uDiv(LValue left, LValue right) { return binary(Machine::UDiv, left, right); }
    LValue mod(LValue left, LValue right) { return binary(Machine::Mod, left, right); }
    LValue chillDiv(LValue left, LValue right) { return chill(Machine::Div, left, right); }
    LValue neg(LValue value) { return unary(Machine::Neg, value); }

    LValue doubleAdd(LValue left, LValue right) { return add(left, right); }
    LValue doubleSub(LValue left, LValue right) { return sub(left, right); }
    LValue doubleMul(LValue left, LValue right) { return mul(left, right); }
    LValue doubleDiv(LValue left, LValue right) { return div(left, right); }
    LValue doubleNeg(LValue value) { return neg(value); }
    LValue doubleAbs(LValue value) { return unary(Machine::Abs, value); }
    LValue doubleCeil(LValue value) { return unary(Machine::Ceil, value); }
    LValue doubleFloor(LValue value) { return unary(Machine::Floor, value); }
    LValue doubleTrunc(LValue value) { return unary(Machine::FTrunc, value); }
    LValue doubleSqrt(LValue value) { return unary(Machine::Sqrt, value); }
    LValue doubleMax(LValue left, LValue right) { return binary(Machine::FMax, left, right); }
    LValue doubleMin(LValue left, LValue right) { return binary(Machine::FMin, left, right); }

    LValue bitAnd(LValue left, LValue right) { return binary(Machine::BitAnd, left, right); }
    LValue bitOr(LValue left, LValue right) { return binary(Machine::BitOr, left, right); }
    LValue bitXor(LValue left, LValue right) { return binary(Machine::BitXor, left, right); }
    LValue shl(LValue value, LValue amount) { return binary(Machine::Shl, value, amount); }
    LValue aShr(LValue value, LValue amount) { return binary(Machine::SShr, value, amount); }
    LValue lShr(LValue value, LValue amount) { return binary(Machine::ZShr, value, amount); }
    LValue logicalNot(LValue value) { return equal(value, int32Zero); }
    LValue ctlz32(LValue value) { return unary(Machine::Clz, value); }
    LValue popCount(LValue value) { return unary(Machine::PopCount, value); }

    LValue signExt32To64(LValue value) { return unary(Machine::SExt32, value); }
    LValue zeroExt(LValue value, LType type) { return value->type() == type ? value : unary(Machine::ZExt32, value); }
    LValue zeroExtPtr(LValue value) { return zeroExt(value, Machine::Int64); }
    LValue intToDouble(LValue value) { return unary(Machine::IToD, value); }
    LValue castToInt32(LValue value) { return value->type() == Machine::Int32 ? value : unary(Machine::Trunc, value); }
    LValue doubleToFloat(LValue value) { return unary(Machine::DoubleToFloat, value); }
    LValue floatToDouble(LValue value) { return unary(Machine::FloatToDouble, value); }
    LValue bitCast(LValue value, LType type) { return bitwiseCast(value, type); }
    LValue fround(LValue value) { return floatToDouble(doubleToFloat(value)); }

    LValue equal(LValue left, LValue right) { return binary(Machine::Equal, left, right); }
    LValue notEqual(LValue left, LValue right) { return binary(Machine::NotEqual, left, right); }
    LValue above(LValue left, LValue right) { return binary(Machine::Above, left, right); }
    LValue aboveOrEqual(LValue left, LValue right) { return binary(Machine::AboveEqual, left, right); }
    LValue below(LValue left, LValue right) { return binary(Machine::Below, left, right); }
    LValue belowOrEqual(LValue left, LValue right) { return binary(Machine::BelowEqual, left, right); }
    LValue greaterThan(LValue left, LValue right) { return binary(Machine::GreaterThan, left, right); }
    LValue greaterThanOrEqual(LValue left, LValue right) { return binary(Machine::GreaterEqual, left, right); }
    LValue lessThan(LValue left, LValue right) { return binary(Machine::LessThan, left, right); }
    LValue lessThanOrEqual(LValue left, LValue right) { return binary(Machine::LessEqual, left, right); }

    LValue doubleEqual(LValue left, LValue right) { return equal(left, right); }
    LValue doubleEqualOrUnordered(LValue left, LValue right) { return binary(Machine::EqualOrUnordered, left, right); }
    LValue doubleNotEqualOrUnordered(LValue left, LValue right) { return notEqual(left, right); }
    LValue doubleNotEqualAndOrdered(LValue left, LValue right) { return logicalNot(doubleEqualOrUnordered(left, right)); }
    LValue doubleLessThan(LValue left, LValue right) { return lessThan(left, right); }
    LValue doubleLessThanOrEqual(LValue left, LValue right) { return lessThanOrEqual(left, right); }
    LValue doubleGreaterThan(LValue left, LValue right) { return greaterThan(left, right); }
    LValue doubleGreaterThanOrEqual(LValue left, LValue right) { return greaterThanOrEqual(left, right); }

    LValue isZero32(LValue value) { return equal(value, int32Zero); }
    LValue notZero32(LValue value) { return notEqual(value, int32Zero); }
    LValue isZero64(LValue value) { return equal(value, int64Zero); }
    LValue notZero64(LValue value) { return notEqual(value, int64Zero); }
    LValue isNull(LValue value) { return isZero64(value); }
    LValue notNull(LValue value) { return notZero64(value); }
    LValue testIsZero32(LValue value, LValue mask) { return isZero32(bitAnd(value, mask)); }
    LValue testNonZero32(LValue value, LValue mask) { return notZero32(bitAnd(value, mask)); }
    LValue testIsZero64(LValue value, LValue mask) { return isZero64(bitAnd(value, mask)); }
    LValue testNonZero64(LValue value, LValue mask) { return notZero64(bitAnd(value, mask)); }
    LValue testIsZeroPtr(LValue value, LValue mask) { return isNull(bitAnd(value, mask)); }
    LValue testNonZeroPtr(LValue value, LValue mask) { return notNull(bitAnd(value, mask)); }

    template<typename T> static constexpr bool isImmediate = std::is_integral_v<T> || std::is_enum_v<T>;
    LValue constantOfTypeOf(LValue value, int64_t constant) { return constantOfType(value->type(), constant); }
#define AOT_DEFINE_OPERATION_WITH_IMMEDIATE(name) \
    template<typename T> requires isImmediate<T> LValue name(LValue left, T right) { return name(left, constantOfTypeOf(left, static_cast<int64_t>(right))); }
    AOT_DEFINE_OPERATION_WITH_IMMEDIATE(add)
    AOT_DEFINE_OPERATION_WITH_IMMEDIATE(sub)
    AOT_DEFINE_OPERATION_WITH_IMMEDIATE(mul)
    AOT_DEFINE_OPERATION_WITH_IMMEDIATE(bitAnd)
    AOT_DEFINE_OPERATION_WITH_IMMEDIATE(bitOr)
    AOT_DEFINE_OPERATION_WITH_IMMEDIATE(bitXor)
    AOT_DEFINE_OPERATION_WITH_IMMEDIATE(equal)
    AOT_DEFINE_OPERATION_WITH_IMMEDIATE(notEqual)
    AOT_DEFINE_OPERATION_WITH_IMMEDIATE(above)
    AOT_DEFINE_OPERATION_WITH_IMMEDIATE(aboveOrEqual)
    AOT_DEFINE_OPERATION_WITH_IMMEDIATE(below)
    AOT_DEFINE_OPERATION_WITH_IMMEDIATE(belowOrEqual)
    AOT_DEFINE_OPERATION_WITH_IMMEDIATE(greaterThan)
    AOT_DEFINE_OPERATION_WITH_IMMEDIATE(greaterThanOrEqual)
    AOT_DEFINE_OPERATION_WITH_IMMEDIATE(lessThan)
    AOT_DEFINE_OPERATION_WITH_IMMEDIATE(lessThanOrEqual)
    AOT_DEFINE_OPERATION_WITH_IMMEDIATE(testIsZero32)
    AOT_DEFINE_OPERATION_WITH_IMMEDIATE(testNonZero32)
    AOT_DEFINE_OPERATION_WITH_IMMEDIATE(testIsZero64)
    AOT_DEFINE_OPERATION_WITH_IMMEDIATE(testNonZero64)
    AOT_DEFINE_OPERATION_WITH_IMMEDIATE(testIsZeroPtr)
    AOT_DEFINE_OPERATION_WITH_IMMEDIATE(testNonZeroPtr)
#undef AOT_DEFINE_OPERATION_WITH_IMMEDIATE
    template<typename T> requires isImmediate<T> LValue shl(LValue value, T amount) { return shl(value, constInt32(static_cast<int32_t>(amount))); }
    template<typename T> requires isImmediate<T> LValue aShr(LValue value, T amount) { return aShr(value, constInt32(static_cast<int32_t>(amount))); }
    template<typename T> requires isImmediate<T> LValue lShr(LValue value, T amount) { return lShr(value, constInt32(static_cast<int32_t>(amount))); }

    LValue valueOf(TypedPointer pointer) { return pointer.offset() ? add(pointer.base(), constIntPtr(pointer.offset())) : pointer.base(); }
    TypedPointer address(const Machine::AbstractHeap& heap, LValue base, ptrdiff_t offset = 0) { return TypedPointer(heap, base, offset); }
    TypedPointer address(LValue base, const Machine::AbstractHeap& field, ptrdiff_t offset = 0) { return TypedPointer(field, base, offset + field.offset()); }
    LValue baseIndex(LValue base, LValue index, Scale scale, ptrdiff_t offset = 0)
    {
        LValue sum = add(base, logScale(scale) ? shl(index, logScale(scale)) : index);
        return offset ? add(sum, constIntPtr(offset)) : sum;
    }
    TypedPointer baseIndex(const Machine::AbstractHeap& heap, LValue base, LValue index, Scale scale, ptrdiff_t offset = 0) { return TypedPointer(heap, baseIndex(base, index, scale), offset); }
    TypedPointer baseIndex(const Machine::IndexedAbstractHeap& heap, LValue base, LValue index, JSValue indexAsConstant = JSValue(), ptrdiff_t offset = 0, LValue mask = nullptr)
    {
        if (indexAsConstant.isInt32())
            return address(base, heap.at(indexAsConstant.asInt32()), offset);
        if (mask)
            index = bitAnd(mask, index);
        return TypedPointer(heap.atAnyIndex(), add(base, mul(index, constIntPtr(heap.elementSize()))), heap.offset() + offset);
    }

    LValue load(TypedPointer pointer, LType type) { return loadAs(Machine::Load, type, pointer, Trapping::No); }
    LValue load8SignExt32(TypedPointer pointer) { return loadAs(Machine::Load8S, Machine::Int32, pointer, Trapping::No); }
    LValue load8ZeroExt32(TypedPointer pointer) { return loadAs(Machine::Load8Z, Machine::Int32, pointer, Trapping::No); }
    LValue load16SignExt32(TypedPointer pointer) { return loadAs(Machine::Load16S, Machine::Int32, pointer, Trapping::No); }
    LValue load16ZeroExt32(TypedPointer pointer) { return loadAs(Machine::Load16Z, Machine::Int32, pointer, Trapping::No); }
    LValue load32(TypedPointer pointer) { return load(pointer, Machine::Int32); }
    LValue load64(TypedPointer pointer) { return load(pointer, Machine::Int64); }
    LValue loadPtr(TypedPointer pointer) { return load(pointer, Machine::pointerType()); }
    LValue loadFloat(TypedPointer pointer) { return load(pointer, Machine::Float); }
    LValue loadDouble(TypedPointer pointer) { return load(pointer, Machine::Double); }
    LValue load8ZeroExt32Trapping(TypedPointer pointer) { return loadAs(Machine::Load8Z, Machine::Int32, pointer, Trapping::Yes); }
    std::optional<int64_t> valueKnownToBeReadThrough(TypedPointer pointer) { return Machine::Builder::valueKnownToBeReadThrough(pointer.base()); }
    LValue load8SignExt32(LValue base, const Machine::AbstractHeap& field) { return load8SignExt32(address(base, field)); }
    LValue load8ZeroExt32(LValue base, const Machine::AbstractHeap& field) { return load8ZeroExt32(address(base, field)); }
    LValue load16SignExt32(LValue base, const Machine::AbstractHeap& field) { return load16SignExt32(address(base, field)); }
    LValue load16ZeroExt32(LValue base, const Machine::AbstractHeap& field) { return load16ZeroExt32(address(base, field)); }
    LValue load32(LValue base, const Machine::AbstractHeap& field) { return load32(address(base, field)); }
    LValue load64(LValue base, const Machine::AbstractHeap& field) { return load64(address(base, field)); }
    LValue loadPtr(LValue base, const Machine::AbstractHeap& field) { return loadPtr(address(base, field)); }
    LValue loadDouble(LValue base, const Machine::AbstractHeap& field) { return loadDouble(address(base, field)); }
    LValue load8ZeroExt32Trapping(LValue base, const Machine::AbstractHeap& field) { return load8ZeroExt32Trapping(address(base, field)); }

    LValue store(LValue value, TypedPointer pointer) { return storeAs(Machine::Store, value, pointer); }
    LValue store32As8(LValue value, TypedPointer pointer) { return storeAs(Machine::Store8, value, pointer); }
    LValue store32As16(LValue value, TypedPointer pointer) { return storeAs(Machine::Store16, value, pointer); }
    LValue store32(LValue value, TypedPointer pointer)
    {
        ASSERT(value->type() == Machine::Int32);
        return store(value, pointer);
    }
    LValue store64(LValue value, TypedPointer pointer)
    {
        ASSERT(value->type() == Machine::Int64);
        return store(value, pointer);
    }
    LValue storePtr(LValue value, TypedPointer pointer) { return store64(value, pointer); }
    LValue storeFloat(LValue value, TypedPointer pointer)
    {
        ASSERT(value->type() == Machine::Float);
        return store(value, pointer);
    }
    LValue storeDouble(LValue value, TypedPointer pointer)
    {
        ASSERT(value->type() == Machine::Double);
        return store(value, pointer);
    }
    void store32As8(LValue value, LValue base, const Machine::AbstractHeap& field) { store32As8(value, address(base, field)); }
    void store32As16(LValue value, LValue base, const Machine::AbstractHeap& field) { store32As16(value, address(base, field)); }
    void store32(LValue value, LValue base, const Machine::AbstractHeap& field) { store32(value, address(base, field)); }
    void store64(LValue value, LValue base, const Machine::AbstractHeap& field) { store64(value, address(base, field)); }
    void storePtr(LValue value, LValue base, const Machine::AbstractHeap& field) { storePtr(value, address(base, field)); }
    void storeDouble(LValue value, LValue base, const Machine::AbstractHeap& field) { storeDouble(value, address(base, field)); }

    using Machine::Builder::fence;

    using Machine::Builder::branch;
    void branch(LValue condition, BranchTargets targets) { branch(condition, targets.taken, targets.notTaken); }
    void unreachable() { oops(); }
    void appendSuccessor(BlockEdge target) { m_block->appendSuccessor(target); }

    Machine::PatchOperation* patchpoint(LType type) { return patch(type); }
    Machine::PatchOperation* patchpointWithUniqueRecord(LType type) { return patch(type); }
    Machine::PatchOperation* checkIsZero(LValue failureCondition) { return check(failureCondition); }

    LValue relocatable(SymbolSpace space, uint32_t index, uint16_t entry = primaryEntry) { return addressOf(CodeSymbol { space, index, entry }); }
    LValue number(uint32_t mention) { return numberOf(CodeSymbol { SymbolSpace::Number, mention }); }
    LValue wideNumber(uint32_t mention) { return numberOf(CodeSymbol { SymbolSpace::Number, mention }, Machine::Int64); }
    TypedPointer instanceWord(const Machine::AbstractHeap& heap, uint32_t mention) { return TypedPointer(heap, instanceAddressOf(CodeSymbol { SymbolSpace::InstanceWord, mention })); }

    uint32_t addConstantData(std::span<const uint8_t> bytes, ConstantKind kind) { return m_proc.addConstantData(bytes, alignmentOfConstant(bytes.size()), kind); }

    void noteForm(Typed::Form form, Typed::StateOfKnowledge state, Typed::Place place = Typed::Place::Anywhere)
    {
        if (m_formsSaid) [[unlikely]]
            m_formsSaid->say(form, state, place);
    }
    void noteFormOf(const Typed::OpcodeSet& operations, Typed::Form form, Typed::StateOfKnowledge state, Typed::Place place = Typed::Place::Anywhere)
    {
        if (!m_formsSaid) [[likely]]
            return;
        std::optional<Typed::Opcode> opcode = m_formsSaid->opcodeBeingGenerated();
        if (opcode && operations.contains(*opcode))
            m_formsSaid->say(form, state, place);
    }
    void noteFormsIn(Typed::FormsSaid& forms) { m_formsSaid = &forms; }

private:
    LValue loadAs(Machine::Opcode opcode, LType type, TypedPointer pointer, Trapping trapping) { return Machine::Builder::load(opcode, type, pointer.base(), pointer.offset(), *pointer.heap(), trapping); }
    LValue storeAs(Machine::Opcode opcode, LValue value, TypedPointer pointer) { return Machine::Builder::store(opcode, value, pointer.base(), pointer.offset(), *pointer.heap()); }

    Typed::FormsSaid* m_formsSaid { nullptr };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
