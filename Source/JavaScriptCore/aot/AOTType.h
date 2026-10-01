/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#if ENABLE(FTL_JIT)

#include "JSCJSValue.h"
#include "JSType.h"
#include "SpeculatedType.h"
#include <wtf/MathExtras.h>
#include <wtf/PrintStream.h>
#include <atomic>

namespace JSC { namespace AOT {

// What the static compiler knows about a value. Unlike a SpeculatedType this is never a guess: a value whose type is T has
// been proven to be in T, by a type check that ran, by the operation that made it, or by both. The bits partition the values
// a virtual register can hold.
//
// Above the bits that say what kind of thing a value is (the tags) are two that say WHICH: which function it is, and which layout of the
// table of types an object was born as (TypeTable). Each is a number, written so that types can go on being joined with `|`: a
// bit of the number takes two bits here, 01 if it is 0 and 10 if it is 1. So what two different numbers join to has 11 somewhere.
// For a function that means there is more than one it may be. For a layout the bits that are 11 are ones that are not known,
// which leaves a lowest and a highest that it may be. Nothing at all: there is no such value. And `&` gives what both allow.
using Type = unsigned __int128;

static constexpr Type TNone = 0;
static constexpr Type TInt32 = Type(1) << 0; // A number that is encoded as an int32.
static constexpr Type TDouble = Type(1) << 1; // A number that is encoded as a double (its value may still be integral).
static constexpr Type TBoolean = Type(1) << 2;
static constexpr Type TUndefined = Type(1) << 3;
static constexpr Type TNull = Type(1) << 4;
// A string that is an atom: in one piece, and the only one there is that says what it says. So two of them are the same string if they are the same StringImpl. What the program
// spells out is one, and so is what is in a field whose type is a union of such. It goes wherever types go: through parameters, results, variables.
static constexpr Type TAtomString = Type(1) << 5;
// One that is not known for an atom: of at most TypedLayoutTable::maxLengthOfAtomizedString characters, or of more. In a field whose strings are atoms there is none of the first kind. So what
// is read from one is the same as a short atom if it has the same StringImpl, and not otherwise.
static constexpr Type TShortOtherString = Type(1) << 37;
static constexpr Type TLongString = Type(1) << 38;
static constexpr Type TOtherString = TShortOtherString | TLongString;
static constexpr Type TString = TAtomString | TOtherString;
static constexpr Type TSymbol = Type(1) << 6;
static constexpr Type TBigInt = Type(1) << 7;
static constexpr unsigned numberOfTagBits = 40;
static constexpr Type TAllTags = (Type(1) << numberOfTagBits) - 1;
static constexpr unsigned bitsOfFunctionNumber = 18;
static constexpr unsigned firstBitOfFunctionNumber = numberOfTagBits;
static constexpr Type TWhicheverFunction = ((Type(1) << 2 * bitsOfFunctionNumber) - 1) << firstBitOfFunctionNumber;
static constexpr unsigned bitsOfLayoutNumber = 16;
static constexpr unsigned firstBitOfLayoutNumber = firstBitOfFunctionNumber + 2 * bitsOfFunctionNumber;
static constexpr Type TWhicheverLayout = ((Type(1) << 2 * bitsOfLayoutNumber) - 1) << firstBitOfLayoutNumber;
static_assert(firstBitOfLayoutNumber + 2 * bitsOfLayoutNumber <= 128);
static constexpr Type TFunctionTag = Type(1) << 8;
static constexpr Type TFunction = TFunctionTag | TWhicheverFunction; // JSFunctionType or InternalFunctionType.
static constexpr Type TArray = Type(1) << 9; // ArrayType or DerivedArrayType.
static constexpr Type TOtherObject = Type(1) << 10; // Every object that is none of the others. It may be one that can be called.
static constexpr Type TCellOther = Type(1) << 11; // Cells that are not JS values, which bytecode passes around (SymbolTable, ...).
static constexpr Type TEmpty = Type(1) << 12; // The hole: a binding in its temporal dead zone.
// Typed arrays, one bit for each type, in the order of the JSTypes.
static constexpr unsigned firstTypedArrayBit = 13;
static constexpr Type TTypedArray = ((Type(1) << NumberOfTypedArrayTypesExcludingDataView) - 1) << firstTypedArrayBit;
static constexpr unsigned firstBitAfterTypedArrays = 25;
static_assert(firstTypedArrayBit + NumberOfTypedArrayTypesExcludingDataView <= firstBitAfterTypedArrays);
// Objects that are told apart by their JSType. None of them can be called.
static constexpr Type TFinalObjectTag = Type(1) << 25;
static constexpr Type TFinalObject = TFinalObjectTag | TWhicheverLayout; // What an object literal makes, and `new` of a function or a class that extends nothing.
static constexpr Type TMap = Type(1) << 26;
static constexpr Type TSet = Type(1) << 27;
static constexpr Type TWeakMap = Type(1) << 28;
static constexpr Type TWeakSet = Type(1) << 29;
static constexpr Type TRegExp = Type(1) << 30;
static constexpr Type TPromise = Type(1) << 31;
static constexpr Type TDate = Type(1) << 32;
static constexpr Type TError = Type(1) << 33;
static constexpr Type TArrayBuffer = Type(1) << 34;
static constexpr Type TDataView = Type(1) << 35;
static constexpr Type TStringObject = Type(1) << 36;
// Every object that is not a function, an array or a typed array.
static constexpr Type TObject = TOtherObject | TFinalObject | TMap | TSet | TWeakMap | TWeakSet | TRegExp | TPromise | TDate | TError | TArrayBuffer | TDataView | TStringObject;
constexpr Type typeOfTypedArray(JSType type) { return Type(1) << (firstTypedArrayBit + type - FirstTypedArrayType); }
constexpr Type typeOfObjectOfKind(JSType); // Of a kind that a mask of op_check_type can name.

struct KindOfObject {
    Type type;
    JSType jsType;
};
static constexpr KindOfObject kindsOfObject[] = {
    { TFinalObject, FinalObjectType }, { TMap, JSMapType }, { TSet, JSSetType }, { TWeakMap, JSWeakMapType }, { TWeakSet, JSWeakSetType }, { TRegExp, RegExpObjectType },
    { TPromise, JSPromiseType }, { TDate, JSDateType }, { TError, ErrorInstanceType }, { TArrayBuffer, ArrayBufferType }, { TDataView, DataViewType }, { TStringObject, StringObjectType },
};
constexpr Type typeOfObjectOfKind(JSType jsType)
{
    for (auto& kind : kindsOfObject) {
        if (kind.jsType == jsType)
            return kind.type;
    }
    return typeOfTypedArray(jsType);
}

static constexpr Type TNumber = TInt32 | TDouble;
static constexpr Type TOther = TUndefined | TNull;
static constexpr Type TAnyObject = TFunction | TArray | TObject | TTypedArray;
static constexpr Type TCell = TString | TSymbol | TBigInt | TAnyObject | TCellOther;
static constexpr Type TPrimitive = TNumber | TBoolean | TOther | TString | TSymbol | TBigInt;
static constexpr Type TTop = TPrimitive | TAnyObject | TCellOther; // Any value a program can see.
static constexpr Type TAll = TTop | TEmpty;

constexpr Type numberOnRails(uint32_t number, unsigned firstBit, unsigned bits)
{
    Type result = 0;
    for (unsigned i = 0; i < bits; ++i)
        result |= Type(number >> i & 1 ? 2 : 1) << (firstBit + 2 * i);
    return result;
}
// The lowest and the highest that the rails allow. If some bit has neither rail there is no such value, and lowest > highest.
struct NumbersOnRails {
    uint32_t lowest;
    uint32_t highest;
    bool isOne() const { return lowest == highest; }
    bool isNone() const { return lowest > highest; }
};
constexpr NumbersOnRails numbersOnRails(Type type, unsigned firstBit, unsigned bits)
{
    NumbersOnRails result { 0, 0 };
    for (unsigned i = 0; i < bits; ++i) {
        unsigned rails = static_cast<unsigned>(type >> (firstBit + 2 * i)) & 3;
        if (!rails)
            return { 1, 0 };
        if (rails == 2)
            result.lowest |= 1u << i;
        if (rails & 2)
            result.highest |= 1u << i;
    }
    return result;
}
// Functions and layouts are numbered from 1.
constexpr Type typeOfFunction(uint32_t number) { return TFunctionTag | numberOnRails(number, firstBitOfFunctionNumber, bitsOfFunctionNumber); }
constexpr Type typeOfObjectWithLayout(uint32_t layout) { return TFinalObjectTag | numberOnRails(layout, firstBitOfLayoutNumber, bitsOfLayoutNumber); }
// One of first to last. (And, as it may be, some others: what the two have in common, from the top bit down, is all that can be said.)
constexpr Type typeOfObjectWithLayoutInRange(uint32_t first, uint32_t last)
{
    Type result = TFinalObjectTag;
    bool differ = false;
    for (unsigned i = bitsOfLayoutNumber; i--;) {
        differ |= (first >> i & 1) != (last >> i & 1);
        result |= Type(differ ? 3 : first >> i & 1 ? 2 : 1) << (firstBitOfLayoutNumber + 2 * i);
    }
    return result;
}
// The one function that the value is, if it is a function. Zero: there is no telling.
constexpr uint32_t functionThatIs(Type type)
{
    auto numbers = numbersOnRails(type, firstBitOfFunctionNumber, bitsOfFunctionNumber);
    return (type & TFunctionTag) && numbers.isOne() ? numbers.lowest : 0;
}
// What the value was born as, if it is what a literal or a constructor makes. 0 among them: it may be of no layout at all.
constexpr NumbersOnRails layoutRangeOf(Type type) { return numbersOnRails(type, firstBitOfLayoutNumber, bitsOfLayoutNumber); }

inline bool isSubtype(Type type, Type of) { return !(type & ~of); }
inline bool mayBe(Type type, Type what) { return type & what; }

// How a value is held in machine code. It follows from the type: nothing is ever unboxed on a guess.
enum class Rep : uint8_t {
    JSValue, // Int64, the EncodedJSValue.
    Int32, // Int32.
    Int64, // Int64: an integer that a double holds exactly (see IntegerRange), and that an int32 may not.
    Double, // Double. May hold an integral value, and any NaN.
    Boolean, // Int32 that is 0 or 1.
};

// What is known of the value of a number, apart from how it is encoded: that it is an integer between two others, and not
// negative zero. Never beyond where every integer is a double.
struct IntegerRange {
    static constexpr int64_t limit = 1ll << 53;

    static constexpr IntegerRange none() { return { 1, 0 }; } // Of no value at all: nothing has been seen to get here.
    static constexpr IntegerRange unknown() { return { INT64_MIN, INT64_MAX }; }
    static constexpr IntegerRange of(int64_t min, int64_t max) { return { min, max }; }
    static IntegerRange ofNumber(JSValue value)
    {
        if (value.isInt32())
            return of(value.asInt32(), value.asInt32());
        if (!value.isDouble())
            return none(); // It says nothing about numbers, and there is nothing to say.
        double number = value.asDouble();
        if (!(number >= -static_cast<double>(limit) && number <= static_cast<double>(limit)))
            return unknown();
        int64_t integer = static_cast<int64_t>(number);
        if (static_cast<double>(integer) != number || (!integer && std::signbit(number)))
            return unknown();
        return of(integer, integer);
    }

    bool isNone() const { return min > max; }
    bool isKnown() const { return min <= max && min != INT64_MIN; }
    bool contains(int64_t value) const { return min <= value && value <= max; }
    bool fitsInt32() const { return isKnown() && min >= INT32_MIN && max <= INT32_MAX; }
    IntegerRange unionWith(IntegerRange other) const
    {
        if (isNone())
            return other;
        if (other.isNone())
            return *this;
        if (!isKnown() || !other.isKnown())
            return unknown();
        return { std::min(min, other.min), std::max(max, other.max) };
    }
    friend bool operator==(const IntegerRange&, const IntegerRange&) = default;

    int64_t min { 1 };
    int64_t max { 0 };
};

inline Rep repForType(Type type)
{
    if (!type)
        return Rep::JSValue;
    if (isSubtype(type, TInt32))
        return Rep::Int32;
    if (isSubtype(type, TNumber))
        return Rep::Double;
    if (isSubtype(type, TBoolean))
        return Rep::Boolean;
    return Rep::JSValue;
}

// The masks of op_check_type (see soundTypeTag() in SpeculatedType.h). The two do not carve up the objects the same way: a
// callable object that is not a JSFunction or an InternalFunction has the Function tag, and is a TOtherObject here.
enum SoundTypeMaskBits : unsigned {
    MaskUndefined = 1, MaskNull = 2, MaskBoolean = 4, MaskNumber = 8, MaskString = 16, MaskSymbol = 32, MaskBigInt = 64,
    MaskFunction = 128, MaskArray = 256, MaskOtherObject = 512,
};

// Everything that can get past a check.
inline Type typeAcceptedByMask(unsigned mask)
{
    Type result = TNone;
    if (mask & MaskUndefined)
        result |= TUndefined;
    if (mask & MaskNull)
        result |= TNull;
    if (mask & MaskBoolean)
        result |= TBoolean;
    if (mask & MaskNumber)
        result |= TNumber;
    if (mask & MaskString)
        result |= TString;
    if (mask & MaskSymbol)
        result |= TSymbol;
    if (mask & MaskBigInt)
        result |= TBigInt;
    if (mask & MaskFunction)
        result |= TFunction | TOtherObject;
    if (mask & MaskArray)
        result |= TArray;
    if (mask & MaskOtherObject)
        result |= soundTypeMaskNamesTypedArray(mask) ? typeOfObjectOfKind(typedArrayTypeOfSoundTypeMask(mask)) : TObject | TTypedArray;
    return result;
}

// What is certain to get past it.
inline Type typeProvingMask(unsigned mask)
{
    Type result = typeAcceptedByMask(mask);
    if ((mask & (MaskFunction | MaskOtherObject)) != (MaskFunction | MaskOtherObject) || soundTypeMaskNamesTypedArray(mask))
        result &= ~TOtherObject;
    return result;
}

// The one type of typed array that the value is, if it is known to be one.
inline std::optional<JSType> typedArrayTypeOf(Type type)
{
    if (!type || !isSubtype(type, TTypedArray) || !hasOneBitSet(static_cast<uint64_t>(type)))
        return std::nullopt;
    return static_cast<JSType>(FirstTypedArrayType + WTF::ctz(static_cast<uint64_t>(type)) - firstTypedArrayBit);
}

// A type that any thread may add to. It only ever grows.
class AtomicType {
public:
    AtomicType() = default;
    AtomicType(const AtomicType& other) { store(other.load()); }
    AtomicType& operator=(const AtomicType& other) { store(other.load()); return *this; }
    // What is read while somebody joins something to it is what was there, what is going to be, or the tags of the one with the numbers of the other. A tag without its number would read as
    // "there is no telling which", which is more than either. So what of the numbers is not in the same word as the tags is there first: a number without its tag says nothing.
    Type load() const
    {
        uint64_t low = m_low.load(std::memory_order_acquire);
        return static_cast<Type>(m_high.load(std::memory_order_relaxed)) << 64 | low;
    }
    void store(Type type)
    {
        m_high.store(static_cast<uint64_t>(type >> 64), std::memory_order_relaxed);
        m_low.store(static_cast<uint64_t>(type), std::memory_order_release);
    }
    // What it was before. One at a time, if there is anything to add: whoever makes one function into several has to know which they are.
    JS_EXPORT_PRIVATE Type join(Type);

private:
    std::atomic<uint64_t> m_low { 0 };
    std::atomic<uint64_t> m_high { 0 };
};

Type typeOfValue(JSValue);
void dumpType(PrintStream&, Type);
MAKE_PRINT_ADAPTOR(TypeDump, Type, dumpType);

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
