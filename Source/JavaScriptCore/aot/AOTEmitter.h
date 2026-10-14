/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTProgram.h"
#include "AOTRuntime.h"
#include "AOTStubs.h"
#include "AOTType.h"
#include "BytecodeIndex.h"
#include "BytecodeStructs.h"
#include "CallFrame.h"
#include "GetPutInfo.h"
#include "Instruction.h"
#include "LinkTimeConstant.h"
#include "Opcode.h"
#include "ScopeOffset.h"
#include "VirtualRegister.h"
#include "B3AbstractHeapRepository.h"
#include "B3Const32Value.h"
#include "B3Const64Value.h"
#include "B3ConstDoubleValue.h"
#include "B3MemoryValue.h"
#include "B3Procedure.h"
#include "B3ValueInlines.h"
#include "FTLAbbreviatedTypes.h"
#include "FTLOutput.h"
#include "FTLValueFromBlock.h"
#include "FTLWeightedTarget.h"

namespace JSC { namespace AOT {

using FTL::LBasicBlock;
using FTL::LType;
using FTL::LValue;
using FTL::ValueFromBlock;
using FTL::rarely;
using FTL::unsure;
using FTL::usually;

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

class IntegersThatLookLikeAddresses {
public:
    static bool isOne(uint64_t bits) { return bits >= 4 * GB && bits < (1ULL << 47); }
    void add(int64_t value)
    {
        if (isOne(value))
            m_integers.add(value);
    }
    bool contains(int64_t value) const { return m_integers.contains(value); }

private:
    UncheckedKeyHashSet<int64_t, WTF::IntHash<int64_t>, WTF::UnsignedWithZeroKeyHashTraits<int64_t>> m_integers;
};

class Output final : public FTL::Output {
public:
    using FTL::Output::Output;
    using FTL::Output::branch;
    void branch(LValue condition, FTL::WeightedTarget taken, FTL::WeightedTarget notTaken)
    {
        if (m_dropsNextGuard) [[unlikely]] {
            m_dropsNextGuard = false;
            jump(taken.weight().value() ? taken.target() : notTaken.target());
            return;
        }
        if (m_forcesNextGuard) [[unlikely]] {
            m_forcesNextGuard = false;
            jump(taken.weight().value() ? notTaken.target() : taken.target());
            return;
        }
        FTL::Output::branch(condition, taken, notTaken);
    }
    void dropNextGuardForTesting() { m_dropsNextGuard = true; }
    void forceNextGuardForTesting() { m_forcesNextGuard = true; }

    // What is made off the usual path is off it: whoever asks how often a block runs (Typed::Lowering::callEntry()) is told so behind a test or a join in a cold arm too.
    LBasicBlock newBlock()
    {
        LBasicBlock block = FTL::Output::newBlock();
        if (m_block && m_block->frequency() < block->frequency())
            block->setFrequency(m_block->frequency());
        return block;
    }

    LValue constInt32(int32_t value) { return constantAtHead<B3::Const32Value>(m_int32sAtHead, value, value); }
    LValue constInt64(int64_t value) { return constantAtHead<B3::Const64Value>(m_int64sAtHead, value, value); }
    LValue constDouble(double value) { return constantAtHead<B3::ConstDoubleValue>(m_doublesAtHead, std::bit_cast<int64_t>(value), value); }
    template<typename T>
    LValue constIntPtr(T* value) { return constInt64(std::bit_cast<intptr_t>(value)); }
    template<typename T>
    LValue constIntPtr(T value) { return constInt64(static_cast<intptr_t>(value)); }

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
    TypedPointer absolute(const void* address) { return FTL::Output::absolute(address); }

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
    LValue load8SignExt32(LValue base, const B3::AbstractHeap& field) { return load8SignExt32(address(base, field)); }
    LValue load8ZeroExt32(LValue base, const B3::AbstractHeap& field) { return load8ZeroExt32(address(base, field)); }
    LValue load16SignExt32(LValue base, const B3::AbstractHeap& field) { return load16SignExt32(address(base, field)); }
    LValue load16ZeroExt32(LValue base, const B3::AbstractHeap& field) { return load16ZeroExt32(address(base, field)); }
    LValue load32(LValue base, const B3::AbstractHeap& field) { return load32(address(base, field)); }
    LValue load32NonNegative(LValue base, const B3::AbstractHeap& field) { return load32(address(base, field)); }
    LValue load64(LValue base, const B3::AbstractHeap& field) { return load64(address(base, field)); }
    LValue loadPtr(LValue base, const B3::AbstractHeap& field) { return loadPtr(address(base, field)); }
    LValue loadDouble(LValue base, const B3::AbstractHeap& field) { return loadDouble(address(base, field)); }
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

    void noteFoldedConstantsIn(IntegersThatLookLikeAddresses& integers) { m_foldedConstants = &integers; }
    LValue add(LValue left, LValue right) { return noteIfFolded(FTL::Output::add(left, right), left, right); }
    LValue shl(LValue value, LValue amount) { return noteIfFolded(FTL::Output::shl(value, amount), value, amount); }
    LValue aShr(LValue value, LValue amount) { return noteIfFolded(FTL::Output::aShr(value, amount), value, amount); }
    LValue lShr(LValue value, LValue amount) { return noteIfFolded(FTL::Output::lShr(value, amount), value, amount); }

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

    LValue loadAs(B3::Opcode opcode, LType type, TypedPointer pointer)
    {
        auto [base, offset] = baseAndOffsetOf(pointer);
        LValue load = m_block->appendNew<B3::MemoryValue>(m_proc, opcode, type, origin(), base, offset);
        m_heaps->decorateMemory(pointer.heap(), load);
        return load;
    }

    LValue storeAs(B3::Opcode opcode, LValue value, TypedPointer pointer)
    {
        auto [base, offset] = baseAndOffsetOf(pointer);
        LValue store = m_block->appendNew<B3::MemoryValue>(m_proc, opcode, origin(), value, base, offset);
        m_heaps->decorateMemory(pointer.heap(), store);
        return store;
    }

    LValue noteIfFolded(LValue result, LValue left, LValue right)
    {
        if (m_foldedConstants && result->hasInt64() && left->hasInt() && right->hasInt())
            m_foldedConstants->add(result->asInt64());
        return result;
    }

    ConstantsAtHead<int32_t> m_int32sAtHead;
    ConstantsAtHead<int64_t> m_int64sAtHead;
    ConstantsAtHead<int64_t> m_doublesAtHead;
    IntegersThatLookLikeAddresses* m_foldedConstants { nullptr };
    bool m_dropsNextGuard { false };
    bool m_forcesNextGuard { false };
};

class Emitter {
    WTF_MAKE_NONCOPYABLE(Emitter);
protected:
    explicit Emitter(B3::Procedure& proc)
        : m_proc(proc)
        , m_heapsOfThread(B3::AbstractHeapRepository::takeFromThread())
        , m_heaps(*m_heapsOfThread)
        , m_out(proc)
    {
    }

    ~Emitter()
    {
        B3::AbstractHeapRepository::giveBackToThread(WTF::move(m_heapsOfThread));
    }

    void findPinnedRegisters();

    // The tags are in registers, which the back end cannot look into. So what is asked of a constant is answered here.
    LValue isInt32(LValue v) { return v->hasInt64() ? m_out.constBool(static_cast<uint64_t>(v->asInt64()) >= static_cast<uint64_t>(JSValue::NumberTag)) : m_out.aboveOrEqual(v, m_numberTag); }
    LValue isNumber(LValue v) { return v->hasInt64() ? m_out.constBool(v->asInt64() & JSValue::NumberTag) : m_out.testNonZero64(v, m_numberTag); }
    LValue isCell(LValue v) { return v->hasInt64() ? m_out.constBool(!(v->asInt64() & JSValue::NotCellMask)) : m_out.testIsZero64(v, m_notCellMask); }
    LValue isNotCell(LValue v) { return v->hasInt64() ? m_out.constBool(v->asInt64() & JSValue::NotCellMask) : m_out.testNonZero64(v, m_notCellMask); }
    LValue isBoolean(LValue v) { return m_out.testIsZero64(m_out.bitXor(v, m_out.constInt64(JSValue::ValueFalse)), m_out.constInt64(~1)); }
    LValue isOther(LValue v) { return m_out.equal(m_out.bitAnd(v, m_out.constInt64(~JSValue::UndefinedTag)), m_out.constInt64(JSValue::ValueNull)); }
    LValue unboxInt32(LValue v) { return m_out.castToInt32(v); }
    LValue boxInt32(LValue v) { return m_out.add(m_out.zeroExt(v, B3::Int64), m_numberTag); }
    LValue unboxDouble(LValue v) { return v->hasInt64() ? m_out.constDouble(std::bit_cast<double>(static_cast<uint64_t>(v->asInt64()) + static_cast<uint64_t>(JSValue::NumberTag))) : m_out.bitCast(m_out.add(v, m_numberTag), B3::Double); }
    LValue boxDouble(LValue v) { return m_out.sub(m_out.bitCast(v, B3::Int64), m_numberTag); }
    LValue unboxBoolean(LValue v) { return m_out.bitAnd(m_out.castToInt32(v), m_out.constInt32(1)); }
    LValue boxBoolean(LValue v)
    {
        static_assert(JSValue::ValueTrue == (JSValue::ValueFalse | 1));
        return m_out.bitOr(m_out.zeroExt(v, B3::Int64), m_out.constInt64(JSValue::ValueFalse));
    }
    LValue numberToDouble(LValue jsNumber);
    LValue cellType(LValue cell) { return m_out.load8ZeroExt32(cell, m_heaps.JSCell_typeInfoType); }
    LValue isCellOfType(LValue cell, JSType type) { return m_out.equal(cellType(cell), m_out.constInt32(type)); }
    LValue isObjectCell(LValue cell) { return m_out.aboveOrEqual(cellType(cell), m_out.constInt32(ObjectType)); }
    LValue registerOnEntry(Reg);
    LValue structureOf(LValue cell);
    LValue structureWithID(LValue structureID);
    LValue entry(Entry);

    void orElse(LValue condition, LBasicBlock otherwise);
    template<typename Functor> void forEachUpTo(LValue count, const Functor&);

    LValue fixedPointer(ptrdiff_t offset);
    LValue vm() { return m_out.loadPtr(m_instance, m_heaps.AOTInstance_vm); }
    LValue fixed32(ptrdiff_t offset);
    LValue changing32(ptrdiff_t offset);
    LValue fixed64(ptrdiff_t offset);
    LValue changing64(ptrdiff_t offset);

    LValue allocateHeapCell(LValue allocator, LBasicBlock slowPath);
    LValue allocateHeapCell(LValue nonNullAllocator, std::optional<size_t> cellSize, LBasicBlock slowPath);
    LValue allocatorForSize(LValue subspace, LValue size, LBasicBlock slowPath);
    LValue allocatorForSize(LValue subspace, size_t);
    void storeHeader(LValue cell, LValue header);
    void storeStructure(LValue cell, LValue structure);
    void splatWords(LValue base, LValue begin, LValue end, LValue, const B3::AbstractHeap&);
    void mutatorFence();
    struct ArrayValues {
        LValue array;
        LValue butterfly;
    };
    ArrayValues allocateJSArray(LValue publicLength, LValue vectorLength, LValue header, LBasicBlock slowPath);
    static uint32_t arrayTypeInfoBlob(IndexingType);

    LValue isRopeString(LValue string) { return m_out.testNonZeroPtr(m_out.loadPtr(string, m_heaps.JSString_value), m_out.constIntPtr(JSString::isRopeInPointer)); }
    LValue singleCharacterString(LValue character);
    LValue emptyString() { return fixedPointer(Instance::offsetOfEmptyString()); }

    LValue newArrayFromValues(LValue values, LValue count, bool areInt32, LBasicBlock giveUp);
    LValue allocateObject(Instance::InlineAllocation, LBasicBlock giveUp);
    LValue newPromise(LBasicBlock giveUp);
    LValue newResolvedPromise(LValue, LBasicBlock giveUp);
    LValue newArrayWithSize(LValue length, LBasicBlock giveUp);
    struct StringParts {
        LValue base;
        LValue impl;
        LValue length;
        LValue offset;
    };
    StringParts stringParts(LValue string, LBasicBlock giveUp);
    LValue substringOf(LValue string, const StringParts&, LValue from, LValue to, LBasicBlock giveUp);
    LValue stringSlice(LValue string, LValue start, LValue end, LBasicBlock giveUp);
    LValue stringSubstring(LValue string, LValue start, LValue end, LBasicBlock giveUp);
    LValue makeRope(LValue first, LValue second, LValue thirdOrNull, LBasicBlock giveUp);
    LValue int32ToString(LValue, LBasicBlock giveUp);
    LValue stringOrInt32ToString(LValue, LBasicBlock giveUp);
    LValue addStringsOrInt32s(LValue first, LValue second, LBasicBlock giveUp);
    LValue concatenate(LValue values, LValue count, LBasicBlock giveUp);
    LValue isOriginalArray(LValue cell);

    B3::Procedure& m_proc;
    std::unique_ptr<B3::AbstractHeapRepository> m_heapsOfThread;
    B3::AbstractHeapRepository& m_heaps;
    Output m_out;
    LValue m_instance { nullptr };
    LValue m_table { nullptr };
    LValue m_numberTag { nullptr };
    LValue m_notCellMask { nullptr };
    LValue m_cellCageBase { nullptr };
    LValue m_statics { nullptr };
    bool m_leavesNextIntervalToSlowPath { false };
};

void pinRegisters(B3::Procedure&);
void generateHelper(CCallHelpers&, Stub);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
