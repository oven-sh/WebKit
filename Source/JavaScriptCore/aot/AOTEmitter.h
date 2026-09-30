/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(FTL_JIT)

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

// What it takes to say in B3 what the engine's values and objects are made of, whoever it is said for: a function of a program (Lowering), or one of the pieces of code that
// all of them call (Helpers). What is here goes by the names that the FTL has for the same things, and takes what those take, but for this: nothing is known by its
// address. What the FTL knows the address of is found from the Instance.
class Emitter {
    WTF_MAKE_NONCOPYABLE(Emitter);
protected:
    explicit Emitter(B3::Procedure& proc)
        : m_proc(proc)
        , m_out(proc)
    {
    }

    // What is in the same registers throughout, and what is found from that.
    void findWhatIsPinned();

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
    template<typename Functor> void forEachUpTo(LValue count, const Functor&); // An Int32. The functor is given an index the size of a pointer.

    // Of the Instance: what stays as it is once the Instance has been made, and what does not.
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
    // An array that keeps its elements as values, one after the other (or as integers, which look the same): with room for vectorLength, of which the first publicLength are for
    // whoever asks to fill in. The rest have nothing in them.
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

    // ---- What is long enough to be worth having once (Helpers), and short enough to be worth spelling out where code goes round and round. Each goes to giveUp with nothing done
    // if it is not the plain case.
    LValue newArrayOfValues(LValue values, LValue count, bool areInt32, LBasicBlock giveUp); // Int32 count.
    LValue newArrayFromButterfly(LValue immutableButterfly, LBasicBlock giveUp);
    LValue newActivation(LValue scope, LValue symbolTable, LValue initialValue, LValue count, LBasicBlock giveUp);
    LValue newArrayWithSpread(LValue values, LValue count, LValue whichAreToBeSpread, LBasicBlock giveUp);
    LValue newArrayLike(LValue length, LValue array, LBasicBlock giveUp); // What map() and the like put their results in: of that length, a value, with nothing in it yet.
    // Where the characters are: in the string itself, or, if it is a slice, in what it is a slice of. Anything else that has yet to be put together is given up on.
    struct PiecesOfString {
        LValue base; // A string that is all in one piece.
        LValue impl; // Its StringImpl.
        LValue length; // Of the string that was asked about.
        LValue offset; // Where in the base it starts.
    };
    PiecesOfString piecesOfString(LValue string, LBasicBlock giveUp);
    LValue partOfString(LValue string, const PiecesOfString&, LValue from, LValue to, LBasicBlock giveUp);
    LValue sliceOfString(LValue string, LValue start, LValue end, LBasicBlock giveUp); // As slice() and substring() take them, once they are integers.
    LValue substringOfString(LValue string, LValue start, LValue end, LBasicBlock giveUp);
    LValue makeRope(LValue first, LValue second, LValue thirdOrNull, LBasicBlock giveUp);
    LValue keysOfObject(LValue object, LBasicBlock giveUp);
    LValue lowerCaseIfItIsAlready(LValue string, LBasicBlock giveUp);
    void addFieldOfStruct(LValue object, LValue valueAsHeld, LValue slot, LBasicBlock giveUp);
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

// The code of one of the Stub::Helper... stubs: what an Emitter says, compiled once for everybody. It is given what it works on in the argument registers and hands back what it
// made, or nothing if it gave up, having done nothing that shows: then it is for whoever called to have the same done the long way. It calls nothing.
void generateHelper(CCallHelpers&, Stub);

// TEMPORARY: BUN_AOT_WITHOUT=mask leaves these to the runtime, as they used to be.
enum Without : unsigned { WithoutNewArray = 1, WithoutNewArrayBuffer = 2, WithoutNewActivation = 4, WithoutSpread = 8, WithoutRest = 16, WithoutRopes = 32, WithoutSpecies = 64, WithoutPutByValDirect = 128, WithoutIteratorOpen = 256, WithoutIteratorEnd = 512, WithoutAddsOfFields = 1024, WithoutTypeof = 2048 };
bool isWithout(Without);

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
