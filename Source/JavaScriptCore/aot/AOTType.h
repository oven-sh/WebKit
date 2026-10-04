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

#if ENABLE(AOT)

#include "JSCJSValue.h"
#include "JSType.h"
#include "Options.h"
#include "SpeculatedType.h"
#include <wtf/MathExtras.h>
#include <wtf/PrintStream.h>
#include <atomic>
#include <bit>

namespace JSC { namespace AOT {

using Type = unsigned __int128;
inline unsigned numberOfBitsIn(Type type) { return std::popcount(static_cast<uint64_t>(type)) + std::popcount(static_cast<uint64_t>(type >> 64)); }

static constexpr Type TNone = 0;
static constexpr Type TInt32 = Type(1) << 0;
static constexpr Type TDouble = Type(1) << 1;
static constexpr Type TBoolean = Type(1) << 2;
static constexpr Type TUndefined = Type(1) << 3;
static constexpr Type TNull = Type(1) << 4;
static constexpr Type TAtomString = Type(1) << 5;
static constexpr Type TShortOtherString = Type(1) << 37;
static constexpr Type TLongString = Type(1) << 38;
static constexpr Type TOtherString = TShortOtherString | TLongString;
static constexpr Type TString = TAtomString | TOtherString;
static constexpr Type TSymbol = Type(1) << 6;
static constexpr Type TBigInt = Type(1) << 7;
static constexpr unsigned numberOfTagBits = 40;
static constexpr Type TAllTags = (Type(1) << numberOfTagBits) - 1;
static constexpr unsigned functionNumberBits = 20;
static constexpr unsigned firstFunctionNumberBit = numberOfTagBits;
static constexpr Type TAnyFunctionNumber = ((Type(1) << 2 * functionNumberBits) - 1) << firstFunctionNumberBit;
static constexpr unsigned layoutNumberBits = 16;
static constexpr unsigned firstLayoutNumberBit = firstFunctionNumberBit + 2 * functionNumberBits;
static constexpr Type TAnyLayoutNumber = ((Type(1) << 2 * layoutNumberBits) - 1) << firstLayoutNumberBit;
static_assert(firstLayoutNumberBit + 2 * layoutNumberBits <= 128);
static constexpr Type TFunctionTag = Type(1) << 8;
static constexpr Type TFunction = TFunctionTag | TAnyFunctionNumber;
static constexpr Type TArray = Type(1) << 9;
static constexpr Type TOtherObject = Type(1) << 10;
static constexpr Type TCellOther = Type(1) << 11;
static constexpr Type TEmpty = Type(1) << 12;
static constexpr unsigned firstTypedArrayBit = 13;
static constexpr Type TTypedArray = ((Type(1) << NumberOfTypedArrayTypesExcludingDataView) - 1) << firstTypedArrayBit;
static constexpr unsigned firstBitAfterTypedArrays = 25;
static_assert(firstTypedArrayBit + NumberOfTypedArrayTypesExcludingDataView <= firstBitAfterTypedArrays);
static constexpr Type TFinalObjectTag = Type(1) << 25;
static constexpr Type TFinalObject = TFinalObjectTag | TAnyLayoutNumber;
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
static constexpr Type TObject = TOtherObject | TFinalObject | TMap | TSet | TWeakMap | TWeakSet | TRegExp | TPromise | TDate | TError | TArrayBuffer | TDataView | TStringObject;
constexpr Type typeForTypedArray(JSType type) { return Type(1) << (firstTypedArrayBit + type - FirstTypedArrayType); }
constexpr Type objectTypeForKind(JSType);

struct ObjectKind {
    Type type;
    JSType jsType;
};
static constexpr ObjectKind objectKinds[] = {
    { TFinalObject, FinalObjectType }, { TMap, JSMapType }, { TSet, JSSetType }, { TWeakMap, JSWeakMapType }, { TWeakSet, JSWeakSetType }, { TRegExp, RegExpObjectType },
    { TPromise, JSPromiseType }, { TDate, JSDateType }, { TError, ErrorInstanceType }, { TArrayBuffer, ArrayBufferType }, { TDataView, DataViewType }, { TStringObject, StringObjectType },
};
constexpr Type objectTypeForKind(JSType jsType)
{
    for (auto& kind : objectKinds) {
        if (kind.jsType == jsType)
            return kind.type;
    }
    return typeForTypedArray(jsType);
}

static constexpr Type TNumber = TInt32 | TDouble;
static constexpr Type TOther = TUndefined | TNull;
static constexpr Type TAnyObject = TFunction | TArray | TObject | TTypedArray;
static constexpr Type TCell = TString | TSymbol | TBigInt | TAnyObject | TCellOther;
static constexpr Type TPrimitive = TNumber | TBoolean | TOther | TString | TSymbol | TBigInt;
static constexpr Type TTop = TPrimitive | TAnyObject | TCellOther;
static constexpr Type TAll = TTop | TEmpty;

constexpr Type dualRailEncode(uint32_t number, unsigned firstBit, unsigned bits)
{
    Type result = 0;
    for (unsigned i = 0; i < bits; ++i)
        result |= Type(number >> i & 1 ? 2 : 1) << (firstBit + 2 * i);
    return result;
}
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
inline uint32_t maxFunctionNumberInTypes()
{
    constexpr uint32_t largest = (1u << functionNumberBits) - 1;
    uint32_t forTesting = Options::maxAOTFunctionNumberInTypesForTesting();
    return forTesting ? std::min(forTesting, largest) : largest;
}
inline Type functionType(uint32_t number) { return number <= maxFunctionNumberInTypes() ? TFunctionTag | dualRailEncode(number, firstFunctionNumberBit, functionNumberBits) : TFunction; }
constexpr Type objectTypeForLayout(uint32_t layout) { return TFinalObjectTag | dualRailEncode(layout, firstLayoutNumberBit, layoutNumberBits); }
constexpr Type objectTypeForLayoutRange(uint32_t first, uint32_t last)
{
    Type result = TFinalObjectTag;
    bool differ = false;
    for (unsigned i = layoutNumberBits; i--;) {
        differ |= (first >> i & 1) != (last >> i & 1);
        result |= Type(differ ? 3 : first >> i & 1 ? 2 : 1) << (firstLayoutNumberBit + 2 * i);
    }
    return result;
}
constexpr uint32_t functionNumberOf(Type type)
{
    auto numbers = dualRailDecode(type, firstFunctionNumberBit, functionNumberBits);
    return (type & TFunctionTag) && numbers.isOne() ? numbers.lowest : 0;
}
constexpr DualRailNumbers layoutRangeOf(Type type) { return dualRailDecode(type, firstLayoutNumberBit, layoutNumberBits); }

inline bool isSubtype(Type type, Type of) { return !(type & ~of); }
inline bool mayBe(Type type, Type what) { return type & what; }

enum class Rep : uint8_t {
    JSValue,
    Int32,
    Int64,
    Double,
    Boolean,
};

struct IntegerRange {
    static constexpr int64_t limit = 1ll << 53;

    static constexpr IntegerRange none() { return { 1, 0 }; }
    static constexpr IntegerRange unknown() { return { INT64_MIN, INT64_MAX }; }
    static constexpr IntegerRange of(int64_t min, int64_t max) { return { min, max }; }
    static IntegerRange forNumber(JSValue value)
    {
        if (value.isInt32())
            return of(value.asInt32(), value.asInt32());
        if (!value.isDouble())
            return none();
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

enum SoundTypeMaskBits : unsigned {
    MaskUndefined = 1, MaskNull = 2, MaskBoolean = 4, MaskNumber = 8, MaskString = 16, MaskSymbol = 32, MaskBigInt = 64,
    MaskFunction = 128, MaskArray = 256, MaskOtherObject = 512,
};

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
        result |= soundTypeMaskNamesTypedArray(mask) ? objectTypeForKind(typedArrayTypeForSoundTypeMask(mask)) : TObject | TTypedArray;
    return result;
}

inline Type typeProvingMask(unsigned mask)
{
    Type result = typeAcceptedByMask(mask);
    if ((mask & (MaskFunction | MaskOtherObject)) != (MaskFunction | MaskOtherObject) || soundTypeMaskNamesTypedArray(mask))
        result &= ~TOtherObject;
    return result;
}

inline std::optional<JSType> typedArrayTypeOf(Type type)
{
    if (!type || !isSubtype(type, TTypedArray) || !hasOneBitSet(static_cast<uint64_t>(type)))
        return std::nullopt;
    return static_cast<JSType>(FirstTypedArrayType + WTF::ctz(static_cast<uint64_t>(type)) - firstTypedArrayBit);
}

class AtomicType {
public:
    AtomicType() = default;
    AtomicType(const AtomicType& other) { store(other.load()); }
    AtomicType& operator=(const AtomicType& other) { store(other.load()); return *this; }
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
    JS_EXPORT_PRIVATE Type join(Type);

private:
    std::atomic<uint64_t> m_low { 0 };
    std::atomic<uint64_t> m_high { 0 };
};

Type valueType(JSValue);
void dumpType(PrintStream&, Type);
MAKE_PRINT_ADAPTOR(TypeDump, Type, dumpType);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
