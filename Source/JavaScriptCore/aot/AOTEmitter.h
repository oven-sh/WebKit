/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

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

// Emits B3 code that operates on the engine's values and objects. It is shared by the code that compiles a program's functions
// (Lowering) and the code that generates the shared helper stubs (Helpers). The methods have the same names and parameters as their
// FTL counterparts, with one difference: nothing is referred to by its address. What the FTL would embed as an address is loaded
// from the Instance.
class Emitter {
    WTF_MAKE_NONCOPYABLE(Emitter);
protected:
    explicit Emitter(B3::Procedure& proc)
        : m_proc(proc)
        , m_out(proc)
    {
    }

    // Sets up the pinned registers and the values that are derived from them.
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
    LValue numberToDouble(LValue jsNumber); // A boxed value known to be a number.
    LValue cellType(LValue cell) { return m_out.load8ZeroExt32(cell, m_heaps.JSCell_typeInfoType); }
    LValue isCellOfType(LValue cell, JSType type) { return m_out.equal(cellType(cell), m_out.constInt32(type)); }
    LValue isObjectCell(LValue cell) { return m_out.aboveOrEqual(cellType(cell), m_out.constInt32(ObjectType)); }
    LValue registerOnEntry(Reg); // What was in it when the function was called.
    LValue structureOf(LValue cell);
    LValue structureWithID(LValue structureID);
    LValue entry(Entry);

    // Goes on if the condition holds.
    void orElse(LValue condition, LBasicBlock otherwise);
    template<typename Functor> void forEachUpTo(LValue count, const Functor&); // count is an Int32. The functor receives a pointer-sized index.

    // Loads from the Instance: fields that are constant once the Instance has been created, and fields that change.
    LValue fixedPointer(ptrdiff_t offset);
    LValue fixed32(ptrdiff_t offset);
    LValue changing32(ptrdiff_t offset);

    // ---- Allocation. Nothing here calls anything: where there is no room, or nothing to allocate from yet, it goes to slowPath.
    LValue allocateHeapCell(LValue allocator, LBasicBlock slowPath);
    LValue allocatorForSize(LValue subspace, LValue size, LBasicBlock slowPath);
    LValue allocatorForSize(LValue subspace, size_t size);
    void storeHeader(LValue cell, LValue structureID, uint32_t typeInfoBlob); // Of a Structure whose type, flags and way of keeping elements are known.
    void storeStructure(LValue cell, LValue structure);
    void splatWords(LValue base, LValue begin, LValue end, LValue value, const B3::AbstractHeap&); // Int32 indices of words.
    void mutatorFence();
    // An array with contiguous JSValue storage (or Int32 storage, which has the same layout) and capacity vectorLength. The caller
    // has to fill in the first publicLength elements. The rest are holes.
    struct ArrayValues {
        LValue array;
        LValue butterfly;
    };
    ArrayValues allocateJSArray(LValue publicLength, LValue vectorLength, LValue structureID, uint32_t typeInfoBlob, LBasicBlock slowPath);
    static uint32_t typeInfoBlobOfArray(IndexingType);

    // ---- Strings.
    LValue isRopeString(LValue string) { return m_out.testNonZeroPtr(m_out.loadPtr(string, m_heaps.JSString_value), m_out.constIntPtr(JSString::isRopeInPointer)); }
    LValue singleCharacterString(LValue character); // Int32, no more than maxSingleCharacterString.
    LValue emptyString() { return fixedPointer(Instance::offsetOfEmptyString()); }

    // ---- Code sequences that are long enough to share (Helpers), but short enough to emit inline in loops. Each jumps to giveUp,
    // without side effects, if it is not the common case.
    LValue newArrayOfValues(LValue values, LValue count, bool areInt32, LBasicBlock giveUp); // Int32 count.
    LValue newArrayFromButterfly(LValue immutableButterfly, LBasicBlock giveUp);
    LValue newActivation(LValue scope, LValue symbolTable, LValue initialValue, LValue count, LBasicBlock giveUp);
    LValue newArrayWithSpread(LValue values, LValue count, LValue spreadMask, LBasicBlock giveUp);
    LValue newArrayLike(LValue length, LValue array, LBasicBlock giveUp); // The array that map() and similar builtins store their results in. length is a JSValue. The elements are holes.
    // The location of a string's characters: in the string itself or, for a substring rope, in its base string. Any other rope is
    // given up on.
    struct StringParts {
        LValue base; // A string that is not a rope.
        LValue impl; // Its StringImpl.
        LValue length; // The length of the original string.
        LValue offset; // Where in the base it starts.
    };
    StringParts stringParts(LValue string, LBasicBlock giveUp);
    LValue substringOf(LValue string, const StringParts&, LValue from, LValue to, LBasicBlock giveUp);
    LValue sliceOfString(LValue string, LValue start, LValue end, LBasicBlock giveUp); // As slice() and substring() take them, once they are integers.
    LValue substringOfString(LValue string, LValue start, LValue end, LBasicBlock giveUp);
    LValue makeRope(LValue first, LValue second, LValue thirdOrNull, LBasicBlock giveUp);
    LValue addStrings(LValue first, LValue second, LBasicBlock giveUp); // Of values, that may be anything.
    LValue keysOfObject(LValue object, LBasicBlock giveUp);
    LValue stringIfAlreadyLowerCase(LValue string, LBasicBlock giveUp);
    void setLengthOfArray(LValue array, LValue length, LBasicBlock giveUp); // Both are values. To no more than it is.
    void addTypedField(LValue object, LValue storedValue, LValue slot, LBasicBlock giveUp);
    LValue isOriginalArray(LValue cell); // A boolean: see Instance::structureIDsOfOriginalArrays.

    B3::Procedure& m_proc;
    B3::AbstractHeapRepository m_heaps;
    FTL::Output m_out;
    LValue m_instance { nullptr };
    LValue m_vm { nullptr };
    LValue m_globalObject { nullptr };
    LValue m_table { nullptr };
    LValue m_numberTag { nullptr };
    LValue m_notCellMask { nullptr };
};

// Generates the code of one of the Stub::Helper... stubs: an Emitter sequence that is compiled once and shared. It takes its
// operands in the argument registers and returns its result, or null if it gave up. In that case it has had no side effects, and
// the caller has to take the slow path. It makes no calls.
void generateHelper(CCallHelpers&, Stub);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
