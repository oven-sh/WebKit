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
#include "Options.h"
#include "SpeculatedType.h"
#include <wtf/MathExtras.h>
#include <wtf/PrintStream.h>
#include <atomic>

namespace JSC { namespace AOT {

// What the ahead-of-time compiler knows about a value. Unlike a SpeculatedType this is never a prediction: a value of type T has
// been proven to be in T, by a type check that ran, by the operation that produced it, or both. The tag bits partition the set of
// values a virtual register can hold.
//
// Above the tag bits are two numbers: which function the value is, and which typed layout an object was allocated with (TypeTable).
// Each uses a dual-rail encoding, so that types can still be joined with `|`: every bit of the number takes two bits here, 01 for 0
// and 10 for 1. Joining two different numbers therefore produces 11 in some position. For a function number that means more than
// one function is possible. For a layout number the positions that are 11 are unknown bits, which gives a lowest and a highest
// possible value. 00 in any position means there is no such value. `&` computes the intersection.
using Type = unsigned __int128;

static constexpr Type TNone = 0;
static constexpr Type TInt32 = Type(1) << 0; // A number that is encoded as an int32.
static constexpr Type TDouble = Type(1) << 1; // A number that is encoded as a double (its value may still be integral).
static constexpr Type TBoolean = Type(1) << 2;
static constexpr Type TUndefined = Type(1) << 3;
static constexpr Type TNull = Type(1) << 4;
// A string whose StringImpl is an atom: resolved (not a rope) and unique for its contents, so two atom strings are equal exactly
// when their StringImpls are identical. String literals are atoms, and so are the values of fields whose type is a union of string
// literals. This propagates like any other type, through parameters, results and variables.
static constexpr Type TAtomString = Type(1) << 5;
// A string not known to be an atom, split by length: at most TypedLayoutTable::maxLengthOfAtomizedString characters, or longer. A
// field with atomized strings never holds a TShortOtherString, so a value read from one equals a short atom exactly when their
// StringImpls are identical.
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
static constexpr Type TAnyFunctionNumber = ((Type(1) << 2 * bitsOfFunctionNumber) - 1) << firstBitOfFunctionNumber;
static constexpr unsigned bitsOfLayoutNumber = 16;
static constexpr unsigned firstBitOfLayoutNumber = firstBitOfFunctionNumber + 2 * bitsOfFunctionNumber;
static constexpr Type TAnyLayoutNumber = ((Type(1) << 2 * bitsOfLayoutNumber) - 1) << firstBitOfLayoutNumber;
static_assert(firstBitOfLayoutNumber + 2 * bitsOfLayoutNumber <= 128);
static constexpr Type TFunctionTag = Type(1) << 8;
static constexpr Type TFunction = TFunctionTag | TAnyFunctionNumber; // JSFunctionType or InternalFunctionType.
static constexpr Type TArray = Type(1) << 9; // ArrayType or DerivedArrayType.
static constexpr Type TOtherObject = Type(1) << 10; // Any object not covered by another bit. May be callable.
static constexpr Type TCellOther = Type(1) << 11; // Cells that are not JS values but that bytecode passes around (SymbolTable, ...).
static constexpr Type TEmpty = Type(1) << 12; // The empty value: a binding in its temporal dead zone.
// Typed arrays: one bit per type, in JSType order.
static constexpr unsigned firstTypedArrayBit = 13;
static constexpr Type TTypedArray = ((Type(1) << NumberOfTypedArrayTypesExcludingDataView) - 1) << firstTypedArrayBit;
static constexpr unsigned firstBitAfterTypedArrays = 25;
static_assert(firstTypedArrayBit + NumberOfTypedArrayTypesExcludingDataView <= firstBitAfterTypedArrays);
// Objects distinguished by JSType. None of them is callable.
static constexpr Type TFinalObjectTag = Type(1) << 25;
static constexpr Type TFinalObject = TFinalObjectTag | TAnyLayoutNumber; // Created by an object literal, or by `new` on a function or a base class.
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
constexpr Type typeOfObjectOfKind(JSType); // For a JSType that an op_check_type mask can name.

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

constexpr Type dualRailEncode(uint32_t number, unsigned firstBit, unsigned bits)
{
    Type result = 0;
    for (unsigned i = 0; i < bits; ++i)
        result |= Type(number >> i & 1 ? 2 : 1) << (firstBit + 2 * i);
    return result;
}
// The lowest and highest numbers the rails allow. If some bit has neither rail set, there is no such value and lowest > highest.
struct DualRailNumbers {
    uint32_t lowest;
    uint32_t highest;
    bool isOne() const { return lowest == highest; }
    bool isNone() const { return lowest > highest; }
};
constexpr DualRailNumbers dualRailDecode(Type type, unsigned firstBit, unsigned bits)
{
    DualRailNumbers result { 0, 0 };
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
// Function and layout numbers start at 1.
// A program may have more functions than a type has numbers for. One beyond the limit is any function as far as types go. Where it
// goes cannot be followed then, so it is treated as escaping (FunctionSummary::NumberDoesNotFitInTypes).
inline uint32_t largestFunctionNumberInTypes()
{
    constexpr uint32_t largest = (1u << bitsOfFunctionNumber) - 1;
    uint32_t forTesting = Options::largestAOTFunctionNumberInTypesForTesting();
    return forTesting ? std::min(forTesting, largest) : largest;
}
inline Type typeOfFunction(uint32_t number) { return number <= largestFunctionNumberInTypes() ? TFunctionTag | dualRailEncode(number, firstBitOfFunctionNumber, bitsOfFunctionNumber) : TFunction; }
constexpr Type typeOfObjectWithLayout(uint32_t layout) { return TFinalObjectTag | dualRailEncode(layout, firstBitOfLayoutNumber, bitsOfLayoutNumber); }
// A layout in [first, last]. The encoding can only express the high-order bits the two have in common, so the result may admit
// other layouts as well.
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
// The function number, if the value is known to be one specific function. Otherwise zero.
constexpr uint32_t functionNumberOf(Type type)
{
    auto numbers = dualRailDecode(type, firstBitOfFunctionNumber, bitsOfFunctionNumber);
    return (type & TFunctionTag) && numbers.isOne() ? numbers.lowest : 0;
}
// The range of typed layout IDs the value may have, if it is a final object. A range that includes 0 means it may have no typed
// layout.
constexpr DualRailNumbers layoutRangeOf(Type type) { return dualRailDecode(type, firstBitOfLayoutNumber, bitsOfLayoutNumber); }

inline bool isSubtype(Type type, Type of) { return !(type & ~of); }
inline bool mayBe(Type type, Type what) { return type & what; }

// How a value is represented in machine code. This follows from its proven type; nothing is unboxed speculatively.
enum class Rep : uint8_t {
    JSValue, // Int64, the EncodedJSValue.
    Int32, // Int32.
    Int64, // Int64: an integer that a double represents exactly (see IntegerRange) but that may not fit in an int32.
    Double, // Double. May hold an integral value, and any NaN.
    Boolean, // Int32 that is 0 or 1.
};

// What is known about a number's value, independent of its encoding: it is an integer in [min, max] and is not negative zero. The
// bounds stay within the range where every integer is exactly representable as a double.
struct IntegerRange {
    static constexpr int64_t limit = 1ll << 53;

    static constexpr IntegerRange none() { return { 1, 0 }; } // The empty range: no value has been seen yet.
    static constexpr IntegerRange unknown() { return { INT64_MIN, INT64_MAX }; }
    static constexpr IntegerRange of(int64_t min, int64_t max) { return { min, max }; }
    static IntegerRange ofNumber(JSValue value)
    {
        if (value.isInt32())
            return of(value.asInt32(), value.asInt32());
        if (!value.isDouble())
            return none(); // Not a number, so it contributes nothing to the range.
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

// The mask bits of op_check_type (see soundTypeTag() in SpeculatedType.h). Masks and Types classify objects differently: a callable
// object that is not a JSFunction or an InternalFunction has the Function tag but is a TOtherObject here.
enum SoundTypeMaskBits : unsigned {
    MaskUndefined = 1, MaskNull = 2, MaskBoolean = 4, MaskNumber = 8, MaskString = 16, MaskSymbol = 32, MaskBigInt = 64,
    MaskFunction = 128, MaskArray = 256, MaskOtherObject = 512,
};

// The union of all values that can pass the check.
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

// The values that are guaranteed to pass the check.
inline Type typeProvingMask(unsigned mask)
{
    Type result = typeAcceptedByMask(mask);
    if ((mask & (MaskFunction | MaskOtherObject)) != (MaskFunction | MaskOtherObject) || soundTypeMaskNamesTypedArray(mask))
        result &= ~TOtherObject;
    return result;
}

// The typed array type, if the value is known to be one specific type.
inline std::optional<JSType> typedArrayTypeOf(Type type)
{
    if (!type || !isSubtype(type, TTypedArray) || !hasOneBitSet(static_cast<uint64_t>(type)))
        return std::nullopt;
    return static_cast<JSType>(FirstTypedArrayType + WTF::ctz(static_cast<uint64_t>(type)) - firstTypedArrayBit);
}

// A Type that any thread may widen. It only grows.
class AtomicType {
public:
    AtomicType() = default;
    AtomicType(const AtomicType& other) { store(other.load()); }
    AtomicType& operator=(const AtomicType& other) { store(other.load()); return *this; }
    // A load that races with a join may observe the old value, the new value, or the tags of one with the numbers of the other. A
    // tag without its number would decode as "unknown function or layout", which is wider than either value. The high word, which
    // holds the number bits that do not share a word with the tags, is therefore written first and read last. A number without its
    // tag has no meaning, so that combination is harmless.
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
    // Returns the previous value. Joins that change the value are serialized, so that the caller that widens one function number to
    // several learns exactly which function was there before.
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
