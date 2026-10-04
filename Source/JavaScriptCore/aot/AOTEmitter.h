/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTGraph.h"
#include "AOTRuntime.h"
#include "B3AbstractHeapRepository.h"
#include "B3Procedure.h"
#include "FTLAbbreviatedTypes.h"
#include "FTLOutput.h"
#include "FTLValueFromBlock.h"
#include "FTLWeightedTarget.h"

namespace JSC { namespace AOT {

using FTL::LBasicBlock;
using FTL::LType;
using FTL::LValue;
using FTL::TypedPointer;
using FTL::ValueFromBlock;
using FTL::rarely;
using FTL::unsure;
using FTL::usually;

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
        FTL::Output::branch(condition, taken, notTaken);
    }
    void dropNextGuardForTesting() { m_dropsNextGuard = true; }

    void noteFoldedConstantsIn(IntegersThatLookLikeAddresses& integers) { m_foldedConstants = &integers; }
    LValue add(LValue left, LValue right) { return noteIfFolded(FTL::Output::add(left, right), left, right); }
    LValue shl(LValue value, LValue amount) { return noteIfFolded(FTL::Output::shl(value, amount), value, amount); }
    LValue aShr(LValue value, LValue amount) { return noteIfFolded(FTL::Output::aShr(value, amount), value, amount); }
    LValue lShr(LValue value, LValue amount) { return noteIfFolded(FTL::Output::lShr(value, amount), value, amount); }

private:
    LValue noteIfFolded(LValue result, LValue left, LValue right)
    {
        if (m_foldedConstants && result->hasInt64() && left->hasInt() && right->hasInt())
            m_foldedConstants->add(result->asInt64());
        return result;
    }

    IntegersThatLookLikeAddresses* m_foldedConstants { nullptr };
    bool m_dropsNextGuard { false };
};

class Emitter {
    WTF_MAKE_NONCOPYABLE(Emitter);
protected:
    explicit Emitter(B3::Procedure& proc)
        : m_proc(proc)
        , m_out(proc)
    {
    }

    void findPinnedRegisters();

    LValue isInt32(LValue v) { return m_out.aboveOrEqual(v, m_numberTag); }
    LValue isNotInt32(LValue v) { return m_out.below(v, m_numberTag); }
    LValue isNumber(LValue v) { return m_out.testNonZero64(v, m_numberTag); }
    LValue isNotNumber(LValue v) { return m_out.testIsZero64(v, m_numberTag); }
    LValue isCell(LValue v) { return m_out.testIsZero64(v, m_notCellMask); }
    LValue isNotCell(LValue v) { return m_out.testNonZero64(v, m_notCellMask); }
    LValue isBoolean(LValue v) { return m_out.testIsZero64(m_out.bitXor(v, m_out.constInt64(JSValue::ValueFalse)), m_out.constInt64(~1)); }
    LValue isOther(LValue v) { return m_out.equal(m_out.bitAnd(v, m_out.constInt64(~JSValue::UndefinedTag)), m_out.constInt64(JSValue::ValueNull)); }
    LValue unboxInt32(LValue v) { return m_out.castToInt32(v); }
    LValue boxInt32(LValue v) { return m_out.add(m_out.zeroExt(v, B3::Int64), m_numberTag); }
    LValue unboxDouble(LValue v) { return m_out.bitCast(m_out.add(v, m_numberTag), B3::Double); }
    LValue boxDouble(LValue v) { return m_out.sub(m_out.bitCast(v, B3::Int64), m_numberTag); }
    LValue unboxBoolean(LValue v) { return m_out.notZero64(m_out.bitAnd(v, m_out.constInt64(1))); }
    LValue boxBoolean(LValue v) { return m_out.select(v, m_out.constInt64(JSValue::ValueTrue), m_out.constInt64(JSValue::ValueFalse)); }
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
    LValue fixed32(ptrdiff_t offset);
    LValue changing32(ptrdiff_t offset);

    LValue allocateHeapCell(LValue allocator, LBasicBlock slowPath);
    LValue allocatorForSize(LValue subspace, LValue size, LBasicBlock slowPath);
    LValue allocatorForSize(LValue subspace, size_t);
    void storeHeader(LValue cell, LValue structureID, uint32_t typeInfoBlob);
    void storeStructure(LValue cell, LValue structure);
    void splatWords(LValue base, LValue begin, LValue end, LValue, const B3::AbstractHeap&);
    void mutatorFence();
    struct ArrayValues {
        LValue array;
        LValue butterfly;
    };
    ArrayValues allocateJSArray(LValue publicLength, LValue vectorLength, LValue structureID, uint32_t typeInfoBlob, LBasicBlock slowPath);
    static uint32_t arrayTypeInfoBlob(IndexingType);

    LValue isRopeString(LValue string) { return m_out.testNonZeroPtr(m_out.loadPtr(string, m_heaps.JSString_value), m_out.constIntPtr(JSString::isRopeInPointer)); }
    LValue singleCharacterString(LValue character);
    LValue emptyString() { return fixedPointer(Instance::offsetOfEmptyString()); }

    LValue newArrayFromValues(LValue values, LValue count, bool areInt32, LBasicBlock giveUp);
    LValue newArrayFromButterfly(LValue immutableButterfly, LBasicBlock giveUp);
    LValue newActivation(LValue scope, LValue symbolTable, LValue initialValue, LValue count, LBasicBlock giveUp);
    LValue allocateObject(Instance::InlineAllocation, LBasicBlock giveUp);
    LValue newPromise(LBasicBlock giveUp);
    LValue newResolvedPromise(LValue, LBasicBlock giveUp);
    LValue newInternalFieldObject(Instance::InlineAllocation, std::span<const JSValue> initialValues, LBasicBlock giveUp);
    LValue newMapOrSet(Instance::InlineAllocation, LBasicBlock giveUp);
    LValue newArrayWithSpread(LValue values, LValue count, LValue spreadMask, LBasicBlock giveUp);
    LValue newArrayLike(LValue length, LValue array, LBasicBlock giveUp);
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
    LValue addStrings(LValue first, LValue second, LBasicBlock giveUp);
    LValue int32ToString(LValue, LBasicBlock giveUp);
    LValue keysOfObject(LValue, LBasicBlock giveUp);
    LValue stringIfAlreadyLowerCase(LValue string, LBasicBlock giveUp);
    void setArrayLength(LValue array, LValue length, LBasicBlock giveUp);
    void addTypedField(LValue, LValue storedValue, LValue slot, LBasicBlock giveUp);
    LValue isOriginalArray(LValue cell);

    B3::Procedure& m_proc;
    B3::AbstractHeapRepository m_heaps;
    Output m_out;
    LValue m_instance { nullptr };
    LValue m_vm { nullptr };
    LValue m_globalObject { nullptr };
    LValue m_table { nullptr };
    LValue m_numberTag { nullptr };
    LValue m_notCellMask { nullptr };
};

void generateHelper(CCallHelpers&, Stub);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
