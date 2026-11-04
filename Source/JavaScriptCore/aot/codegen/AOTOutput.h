/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRelocation.h"
#include "B3AbstractHeapRepository.h"
#include "B3ArgumentRegValue.h"
#include "B3BasicBlockInlines.h"
#include "B3CheckValue.h"
#include "B3Const32Value.h"
#include "B3Const64Value.h"
#include "B3ConstDoubleValue.h"
#include "B3MemoryValue.h"
#include "B3PatchpointValue.h"
#include "B3Policy.h"
#include "B3Procedure.h"
#include "B3RelocatableValue.h"
#include "B3SwitchValue.h"
#include "B3ValueInlines.h"
#include "FTLAbbreviatedTypes.h"
#include "FTLOutput.h"
#include "FTLValueFromBlock.h"
#include "FTLWeightedTarget.h"
#include "Reg.h"
#include <span>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT {

using FTL::LBasicBlock;
using FTL::LType;
using FTL::LValue;
using FTL::ValueFromBlock;

inline B3::FrequentedBlock normalEdge(LBasicBlock block) { return B3::FrequentedBlock(block, B3::FrequencyClass::Normal); }
inline B3::FrequentedBlock unlikelyEdge(LBasicBlock block) { return B3::FrequentedBlock(block, B3::FrequencyClass::Unlikely); }
inline B3::FrequentedBlock rareEdge(LBasicBlock block) { return B3::FrequentedBlock(block, B3::FrequencyClass::Rare); }

struct BranchTargets {
    B3::FrequentedBlock taken;
    B3::FrequentedBlock notTaken;

    BranchTargets swapped() const { return { notTaken, taken }; }
};

struct SwitchTarget {
    int64_t caseValue;
    B3::FrequentedBlock target;
};

class TypedPointer {
public:
    TypedPointer() = default;
    TypedPointer(const B3::AbstractHeap& heap, LValue base, ptrdiff_t offset = 0)
        : m_heap(&heap)
        , m_base(base)
        , m_offset(offset)
    {
    }
    TypedPointer(FTL::TypedPointer pointer)
        : m_heap(pointer.heap())
        , m_base(pointer.value())
    {
    }

    explicit operator bool() const { return !!m_heap; }
    const B3::AbstractHeap* heap() const { return m_heap; }
    LValue base() const { return m_base; }
    ptrdiff_t offset() const { return m_offset; }

private:
    const B3::AbstractHeap* m_heap { nullptr };
    LValue m_base { nullptr };
    ptrdiff_t m_offset { 0 };
};

class Output final : public FTL::Output {
public:
    using FTL::Output::Output;

    void branch(LValue condition, B3::FrequentedBlock taken, B3::FrequentedBlock notTaken)
    {
        if (condition->hasInt()) {
            jump(condition->asInt() ? taken.block() : notTaken.block());
            return;
        }
        m_block->setSuccessors(taken, notTaken);
        m_block->appendNew<B3::Value>(m_proc, B3::Branch, origin(), condition);
    }
    void branch(LValue condition, BranchTargets targets) { branch(condition, targets.taken, targets.notTaken); }

    void switchOn(LValue value, std::span<const SwitchTarget> cases, B3::FrequentedBlock otherwise)
    {
        if (value->hasInt()) {
            LBasicBlock target = otherwise.block();
            for (const SwitchTarget& switchCase : cases) {
                if (switchCase.caseValue == value->asInt()) {
                    target = switchCase.target.block();
                    break;
                }
            }
            jump(target);
            return;
        }
        B3::SwitchValue* switchValue = m_block->appendNew<B3::SwitchValue>(m_proc, origin(), value);
        switchValue->setFallThrough(otherwise);
        for (const SwitchTarget& switchCase : cases)
            switchValue->appendCase(B3::SwitchCase(switchCase.caseValue, switchCase.target));
    }

    unsigned minCasesForJumpTable() const { return m_proc.policy().switchLowering.minCasesForJumpTable; }

    void appendSuccessor(B3::FrequentedBlock target) { m_block->appendSuccessor(target); }

    void retVoid()
    {
        m_block->clearSuccessors();
        m_block->appendNew<B3::Value>(m_proc, B3::Return, origin());
    }

    LValue argumentRegister(Reg reg) { return m_block->appendNew<B3::ArgumentRegValue>(m_proc, origin(), reg); }
    LValue popCount(LValue value) { return m_block->appendNew<B3::Value>(m_proc, B3::PopCount, origin(), value); }

    B3::CheckValue* checkIsZero(LValue failureCondition) { return speculate(failureCondition); }
    B3::PatchpointValue* patchpointWithUniqueRecord(LType type) { return m_block->appendNew<B3::PatchpointValue>(m_proc, type, origin(), B3::cloningForbidden(B3::Patchpoint)); }

    LValue constInt32(int32_t value) { return constantAtHead<B3::Const32Value>(m_int32sAtHead, value, value); }
    LValue constInt64(int64_t value) { return constantAtHead<B3::Const64Value>(m_int64sAtHead, value, value); }
    LValue constDouble(double value) { return constantAtHead<B3::ConstDoubleValue>(m_doublesAtHead, std::bit_cast<int64_t>(value), value); }
    template<typename T>
    LValue constIntPtr(T value) { return constInt64(static_cast<intptr_t>(value)); }

    // An operand that is written as a number becomes a constant once the operand before it has been appended.
    template<typename T> static constexpr bool isImmediate = std::is_integral_v<T> || std::is_enum_v<T>;
    LValue constantOfTypeOf(LValue value, int64_t constant) { return value->type() == B3::Int32 ? constInt32(static_cast<int32_t>(constant)) : constInt64(constant); }
    using FTL::Output::add;
    template<typename T> requires isImmediate<T> LValue add(LValue left, T right) { return add(left, constantOfTypeOf(left, static_cast<int64_t>(right))); }
    using FTL::Output::sub;
    template<typename T> requires isImmediate<T> LValue sub(LValue left, T right) { return sub(left, constantOfTypeOf(left, static_cast<int64_t>(right))); }
    using FTL::Output::mul;
    template<typename T> requires isImmediate<T> LValue mul(LValue left, T right) { return mul(left, constantOfTypeOf(left, static_cast<int64_t>(right))); }
    using FTL::Output::bitAnd;
    template<typename T> requires isImmediate<T> LValue bitAnd(LValue left, T right) { return bitAnd(left, constantOfTypeOf(left, static_cast<int64_t>(right))); }
    using FTL::Output::bitOr;
    template<typename T> requires isImmediate<T> LValue bitOr(LValue left, T right) { return bitOr(left, constantOfTypeOf(left, static_cast<int64_t>(right))); }
    using FTL::Output::bitXor;
    template<typename T> requires isImmediate<T> LValue bitXor(LValue left, T right) { return bitXor(left, constantOfTypeOf(left, static_cast<int64_t>(right))); }
    using FTL::Output::equal;
    template<typename T> requires isImmediate<T> LValue equal(LValue left, T right) { return equal(left, constantOfTypeOf(left, static_cast<int64_t>(right))); }
    using FTL::Output::notEqual;
    template<typename T> requires isImmediate<T> LValue notEqual(LValue left, T right) { return notEqual(left, constantOfTypeOf(left, static_cast<int64_t>(right))); }
    using FTL::Output::above;
    template<typename T> requires isImmediate<T> LValue above(LValue left, T right) { return above(left, constantOfTypeOf(left, static_cast<int64_t>(right))); }
    using FTL::Output::aboveOrEqual;
    template<typename T> requires isImmediate<T> LValue aboveOrEqual(LValue left, T right) { return aboveOrEqual(left, constantOfTypeOf(left, static_cast<int64_t>(right))); }
    using FTL::Output::below;
    template<typename T> requires isImmediate<T> LValue below(LValue left, T right) { return below(left, constantOfTypeOf(left, static_cast<int64_t>(right))); }
    using FTL::Output::belowOrEqual;
    template<typename T> requires isImmediate<T> LValue belowOrEqual(LValue left, T right) { return belowOrEqual(left, constantOfTypeOf(left, static_cast<int64_t>(right))); }
    using FTL::Output::greaterThan;
    template<typename T> requires isImmediate<T> LValue greaterThan(LValue left, T right) { return greaterThan(left, constantOfTypeOf(left, static_cast<int64_t>(right))); }
    using FTL::Output::greaterThanOrEqual;
    template<typename T> requires isImmediate<T> LValue greaterThanOrEqual(LValue left, T right) { return greaterThanOrEqual(left, constantOfTypeOf(left, static_cast<int64_t>(right))); }
    using FTL::Output::lessThan;
    template<typename T> requires isImmediate<T> LValue lessThan(LValue left, T right) { return lessThan(left, constantOfTypeOf(left, static_cast<int64_t>(right))); }
    using FTL::Output::lessThanOrEqual;
    template<typename T> requires isImmediate<T> LValue lessThanOrEqual(LValue left, T right) { return lessThanOrEqual(left, constantOfTypeOf(left, static_cast<int64_t>(right))); }
    using FTL::Output::testIsZero32;
    template<typename T> requires isImmediate<T> LValue testIsZero32(LValue left, T right) { return testIsZero32(left, constantOfTypeOf(left, static_cast<int64_t>(right))); }
    using FTL::Output::testNonZero32;
    template<typename T> requires isImmediate<T> LValue testNonZero32(LValue left, T right) { return testNonZero32(left, constantOfTypeOf(left, static_cast<int64_t>(right))); }
    using FTL::Output::testIsZero64;
    template<typename T> requires isImmediate<T> LValue testIsZero64(LValue left, T right) { return testIsZero64(left, constantOfTypeOf(left, static_cast<int64_t>(right))); }
    using FTL::Output::testNonZero64;
    template<typename T> requires isImmediate<T> LValue testNonZero64(LValue left, T right) { return testNonZero64(left, constantOfTypeOf(left, static_cast<int64_t>(right))); }
    using FTL::Output::testIsZeroPtr;
    template<typename T> requires isImmediate<T> LValue testIsZeroPtr(LValue left, T right) { return testIsZeroPtr(left, constantOfTypeOf(left, static_cast<int64_t>(right))); }
    using FTL::Output::testNonZeroPtr;
    template<typename T> requires isImmediate<T> LValue testNonZeroPtr(LValue left, T right) { return testNonZeroPtr(left, constantOfTypeOf(left, static_cast<int64_t>(right))); }
    using FTL::Output::shl;
    template<typename T> requires isImmediate<T> LValue shl(LValue value, T amount) { return shl(value, constInt32(static_cast<int32_t>(amount))); }
    using FTL::Output::aShr;
    template<typename T> requires isImmediate<T> LValue aShr(LValue value, T amount) { return aShr(value, constInt32(static_cast<int32_t>(amount))); }
    using FTL::Output::lShr;
    template<typename T> requires isImmediate<T> LValue lShr(LValue value, T amount) { return lShr(value, constInt32(static_cast<int32_t>(amount))); }

    LValue valueOf(TypedPointer pointer) { return pointer.offset() ? FTL::Output::add(pointer.base(), constIntPtr(pointer.offset())) : pointer.base(); }
    TypedPointer address(const B3::AbstractHeap& heap, LValue base, ptrdiff_t offset = 0) { return TypedPointer(heap, base, offset); }
    TypedPointer address(LValue base, const B3::AbstractHeap& field, ptrdiff_t offset = 0) { return TypedPointer(field, base, offset + field.offset()); }
    LValue baseIndex(LValue base, LValue index, FTL::Scale scale, ptrdiff_t offset = 0) { return FTL::Output::baseIndex(base, index, scale, offset); }
    TypedPointer baseIndex(const B3::AbstractHeap& heap, LValue base, LValue index, FTL::Scale scale, ptrdiff_t offset = 0) { return TypedPointer(heap, FTL::Output::baseIndex(base, index, scale), offset); }
    TypedPointer baseIndex(B3::IndexedAbstractHeap& heap, LValue base, LValue index, JSValue indexAsConstant = JSValue(), ptrdiff_t offset = 0, LValue mask = nullptr)
    {
        if (indexAsConstant.isInt32())
            return address(base, heap.at(indexAsConstant.asInt32()), offset);
        if (mask)
            index = bitAnd(mask, index);
        return TypedPointer(heap.atAnyIndex(), FTL::Output::add(base, mul(index, constIntPtr(heap.elementSize()))), heap.offset() + offset);
    }

    LValue load(TypedPointer pointer, LType type) { return loadAs(B3::Load, type, pointer); }
    LValue load8SignExt32(TypedPointer pointer) { return loadAs(B3::Load8S, B3::Int32, pointer); }
    LValue load8ZeroExt32(TypedPointer pointer) { return loadAs(B3::Load8Z, B3::Int32, pointer); }
    LValue load16SignExt32(TypedPointer pointer) { return loadAs(B3::Load16S, B3::Int32, pointer); }
    LValue load16ZeroExt32(TypedPointer pointer) { return loadAs(B3::Load16Z, B3::Int32, pointer); }
    LValue load32(TypedPointer pointer) { return load(pointer, B3::Int32); }
    LValue load32NonNegative(TypedPointer pointer) { return load32(pointer); }
    LValue load64(TypedPointer pointer) { return load(pointer, B3::Int64); }
    LValue loadPtr(TypedPointer pointer) { return load(pointer, B3::pointerType()); }
    LValue loadFloat(TypedPointer pointer) { return load(pointer, B3::Float); }
    LValue loadDouble(TypedPointer pointer) { return load(pointer, B3::Double); }
    LValue load8ZeroExt32Trapping(TypedPointer pointer) { return loadAs(B3::trapping(B3::Load8Z), B3::Int32, pointer); }
    LValue load8SignExt32(LValue base, const B3::AbstractHeap& field) { return load8SignExt32(address(base, field)); }
    LValue load8ZeroExt32(LValue base, const B3::AbstractHeap& field) { return load8ZeroExt32(address(base, field)); }
    LValue load16SignExt32(LValue base, const B3::AbstractHeap& field) { return load16SignExt32(address(base, field)); }
    LValue load16ZeroExt32(LValue base, const B3::AbstractHeap& field) { return load16ZeroExt32(address(base, field)); }
    LValue load32(LValue base, const B3::AbstractHeap& field) { return load32(address(base, field)); }
    LValue load32NonNegative(LValue base, const B3::AbstractHeap& field) { return load32(address(base, field)); }
    LValue load64(LValue base, const B3::AbstractHeap& field) { return load64(address(base, field)); }
    LValue loadPtr(LValue base, const B3::AbstractHeap& field) { return loadPtr(address(base, field)); }
    LValue loadDouble(LValue base, const B3::AbstractHeap& field) { return loadDouble(address(base, field)); }
    LValue load8ZeroExt32Trapping(LValue base, const B3::AbstractHeap& field) { return load8ZeroExt32Trapping(address(base, field)); }
    LValue store(LValue value, TypedPointer pointer) { return storeAs(B3::Store, value, pointer); }
    LValue store32As8(LValue value, TypedPointer pointer) { return storeAs(B3::Store8, value, pointer); }
    LValue store32As16(LValue value, TypedPointer pointer) { return storeAs(B3::Store16, value, pointer); }
    LValue store32(LValue value, TypedPointer pointer) { return store(value, pointer); }
    LValue store64(LValue value, TypedPointer pointer) { return store(value, pointer); }
    LValue storePtr(LValue value, TypedPointer pointer) { return store(value, pointer); }
    LValue storeFloat(LValue value, TypedPointer pointer) { return store(value, pointer); }
    LValue storeDouble(LValue value, TypedPointer pointer) { return store(value, pointer); }
    void store32As8(LValue value, LValue base, const B3::AbstractHeap& field) { store32As8(value, address(base, field)); }
    void store32As16(LValue value, LValue base, const B3::AbstractHeap& field) { store32As16(value, address(base, field)); }
    void store32(LValue value, LValue base, const B3::AbstractHeap& field) { store32(value, address(base, field)); }
    void store64(LValue value, LValue base, const B3::AbstractHeap& field) { store64(value, address(base, field)); }
    void storePtr(LValue value, LValue base, const B3::AbstractHeap& field) { storePtr(value, address(base, field)); }
    void storeDouble(LValue value, LValue base, const B3::AbstractHeap& field) { storeDouble(value, address(base, field)); }

    LValue relocatable(RelocationSpace space, uint32_t index, int64_t addend = 0)
    {
        ASSERT(space != RelocationSpace::B3RuntimeFunction);
        return m_block->appendNew<B3::RelocatableValue>(m_proc, origin(), B3::RelocationSymbol { static_cast<uint32_t>(space), index }, addend);
    }

    uint32_t addConstantData(std::span<const uint8_t> bytes)
    {
        memcpySpan(unsafeMakeSpan(static_cast<uint8_t*>(m_proc.addDataSection(bytes.size())), bytes.size()), bytes);
        return static_cast<uint32_t>(m_proc.dataSections().size() - 1);
    }

    void setFrequency(double) = delete;
    void branch(LValue, LBasicBlock, FTL::Weight, LBasicBlock, FTL::Weight) = delete;
    void branch(LValue, FTL::WeightedTarget, FTL::WeightedTarget) = delete;
    void appendSuccessor(FTL::WeightedTarget) = delete;
    void check(LValue, FTL::WeightedTarget, FTL::Weight) = delete;
    void check(LValue, FTL::WeightedTarget) = delete;
    template<typename VectorType> void switchInstruction(LValue, const VectorType&, LBasicBlock, FTL::Weight) = delete;
    template<typename T> LValue constIntPtr(T*) = delete;
    TypedPointer absolute(const void*) = delete;
    template<typename FunctionType> LValue operation(FunctionType) = delete;
    template<typename Function, typename... Arguments> LValue callWithoutSideEffects(B3::Type, Function, LValue, Arguments...) = delete;
#if ENABLE(FTL_JIT)
    LValue alreadyRegisteredWeakPointer(DFG::Graph&, JSCell*) = delete;
    LValue alreadyRegisteredFrozenPointer(DFG::FrozenValue*) = delete;
    LValue doubleUnary(DFG::Arith::UnaryType, LValue) = delete;
#endif
    LValue doubleStdPow(LValue, LValue) = delete;
    LValue doublePowi(LValue, LValue) = delete;
    void incrementSuperSamplerCount() = delete;
    void decrementSuperSamplerCount() = delete;
    void probeDebugPrint(const String&, LValue) = delete;

private:
    template<typename Key>
    using ConstantsAtHead = UncheckedKeyHashMap<Key, LValue, IntHash<Key>, WTF::UnsignedWithZeroKeyHashTraits<Key>>;

    template<typename ValueType, typename Key, typename Constant>
    LValue constantAtHead(ConstantsAtHead<Key>& constants, Key key, Constant constant)
    {
        if (key >= std::numeric_limits<Key>::max() - 1)
            return m_block->appendNew<ValueType>(m_proc, origin(), constant);
        auto result = constants.add(key, nullptr);
        if (!result.isNewEntry)
            return result.iterator->value;
        LValue value = m_proc.add<ValueType>(origin(), constant);
        auto& atHead = m_proc.at(0)->values();
        if (!atHead.isEmpty() && atHead.last()->effects().terminal)
            atHead.insert(atHead.size() - 1, value);
        else
            atHead.append(value);
        result.iterator->value = value;
        return value;
    }

    std::pair<LValue, int32_t> baseAndOffsetOf(TypedPointer pointer)
    {
        if (pointer.offset() == static_cast<int32_t>(pointer.offset()))
            return { pointer.base(), static_cast<int32_t>(pointer.offset()) };
        return { valueOf(pointer), 0 };
    }

    LValue loadAs(B3::Kind kind, LType type, TypedPointer pointer)
    {
        auto [base, offset] = baseAndOffsetOf(pointer);
        LValue load = m_block->appendNew<B3::MemoryValue>(m_proc, kind, type, origin(), base, offset);
        m_heaps->decorateMemory(pointer.heap(), load);
        return load;
    }

    LValue storeAs(B3::Kind kind, LValue value, TypedPointer pointer)
    {
        auto [base, offset] = baseAndOffsetOf(pointer);
        LValue store = m_block->appendNew<B3::MemoryValue>(m_proc, kind, origin(), value, base, offset);
        m_heaps->decorateMemory(pointer.heap(), store);
        return store;
    }

    ConstantsAtHead<int32_t> m_int32sAtHead;
    ConstantsAtHead<int64_t> m_int64sAtHead;
    ConstantsAtHead<int64_t> m_doublesAtHead;
};

template<typename OutputType, typename Argument>
concept CanCallConstIntPtr = requires(OutputType& out, Argument argument) { out.constIntPtr(argument); };
template<typename OutputType>
concept CanCallAbsolute = requires(OutputType& out, const void* address) { out.absolute(address); };
template<typename OutputType, typename Function>
concept CanCallOperation = requires(OutputType& out, Function function) { out.operation(function); };
template<typename OutputType>
concept CanCallSetFrequency = requires(OutputType& out) { out.setFrequency(1.0); };
template<typename OutputType>
concept CanBranchOnWeights = requires(OutputType& out, LValue condition, FTL::WeightedTarget target) { out.branch(condition, target, target); };

static_assert(CanCallSetFrequency<FTL::Output> && !CanCallSetFrequency<Output>);
static_assert(CanBranchOnWeights<FTL::Output> && !CanBranchOnWeights<Output>);
static_assert(CanCallConstIntPtr<FTL::Output, int*> && !CanCallConstIntPtr<Output, int*>);
static_assert(CanCallAbsolute<FTL::Output> && !CanCallAbsolute<Output>);
static_assert(CanCallOperation<FTL::Output, void (*)()> && !CanCallOperation<Output, void (*)()>);
static_assert(CanCallOperation<FTL::Output, CodePtr<OperationPtrTag>> && !CanCallOperation<Output, CodePtr<OperationPtrTag>>);
static_assert(CanCallConstIntPtr<Output, int>);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
