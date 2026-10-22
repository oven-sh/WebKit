/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSchemaEnums.h"
#include "AOTWire.h"
#include <algorithm>
#include <bit>
#include <cstdint>

namespace JSC { namespace AOT { namespace Typed {

namespace Primitive {
inline constexpr uint16_t Undefined = static_cast<uint16_t>(PrimitiveSet::Undefined);
inline constexpr uint16_t Null = static_cast<uint16_t>(PrimitiveSet::Null);
inline constexpr uint16_t False = static_cast<uint16_t>(PrimitiveSet::False);
inline constexpr uint16_t True = static_cast<uint16_t>(PrimitiveSet::True);
inline constexpr uint16_t Integer = static_cast<uint16_t>(PrimitiveSet::Integer);
inline constexpr uint16_t WholeNumber = static_cast<uint16_t>(PrimitiveSet::WholeNumber);
inline constexpr uint16_t FractionalNumber = static_cast<uint16_t>(PrimitiveSet::FractionalNumber);
inline constexpr uint16_t String = static_cast<uint16_t>(PrimitiveSet::String);
inline constexpr uint16_t Symbol = static_cast<uint16_t>(PrimitiveSet::Symbol);
inline constexpr uint16_t BigInt = static_cast<uint16_t>(PrimitiveSet::BigInt);
inline constexpr uint16_t Absent = static_cast<uint16_t>(PrimitiveSet::Absent);
inline constexpr uint16_t LiteralString = static_cast<uint16_t>(PrimitiveSet::LiteralString);
inline constexpr uint16_t LiteralSymbol = static_cast<uint16_t>(PrimitiveSet::LiteralSymbol);

inline constexpr uint16_t Boolean = False | True;
inline constexpr uint16_t Double = WholeNumber | FractionalNumber;
inline constexpr uint16_t NumberWithoutFraction = Integer | WholeNumber;
inline constexpr uint16_t Number = Integer | Double;
inline constexpr uint16_t UndefinedOrNull = Undefined | Null;
inline constexpr uint16_t AnyString = String | LiteralString;
inline constexpr uint16_t AnySymbol = Symbol | LiteralSymbol;
inline constexpr uint16_t NumberLike = UndefinedOrNull | Boolean | Number;
inline constexpr uint16_t All = UndefinedOrNull | Boolean | Number | String | Symbol | BigInt;
inline constexpr uint16_t Defined = All | Absent | LiteralString | LiteralSymbol;
}

inline constexpr uint16_t cellPrimitives = Primitive::AnyString | Primitive::AnySymbol | Primitive::BigInt;

inline constexpr int64_t maxExactInteger = 1ll << 53;
inline constexpr int64_t maxTrackedInteger = Wire::MaxTrackedInteger;

constexpr bool isTrackedInteger(double number)
{
    if (!(number > -0x1p62 && number < 0x1p62))
        return false;
    if (static_cast<double>(static_cast<int64_t>(number)) != number)
        return false;
    return number || !(std::bit_cast<uint64_t>(number) >> 63);
}

constexpr PrimitiveSet primitiveOfNumber(double number)
{
    if (isTrackedInteger(number))
        return PrimitiveSet::Integer;
    bool fitsInt64 = number > -0x1p63 && number < 0x1p63;
    if (fitsInt64 && static_cast<double>(static_cast<int64_t>(number)) != number)
        return PrimitiveSet::FractionalNumber;
    return PrimitiveSet::WholeNumber;
}

struct Range {
    int64_t minimum { 1 };
    int64_t maximum { 0 };

    static constexpr Range full() { return { -maxTrackedInteger, maxTrackedInteger }; }
    static constexpr Range exact() { return { -maxExactInteger, maxExactInteger }; }
    static constexpr Range int32() { return { INT32_MIN, INT32_MAX }; }
    static constexpr Range uint32() { return { 0, UINT32_MAX }; }
    static constexpr Range length() { return { 0, INT32_MAX }; }
    static constexpr Range empty() { return { 1, 0 }; }
    static constexpr Range one(int64_t value) { return { value, value }; }

    constexpr bool isEmpty() const { return minimum > maximum; }
    constexpr bool isSubsetOf(Range other) const { return isEmpty() || (minimum >= other.minimum && maximum <= other.maximum); }
    constexpr bool contains(int64_t value) const { return minimum <= value && value <= maximum; }
    constexpr bool isSingleValue() const { return minimum == maximum; }
    constexpr Range unionWith(Range other) const
    {
        if (isEmpty())
            return other;
        if (other.isEmpty())
            return *this;
        return { std::min(minimum, other.minimum), std::max(maximum, other.maximum) };
    }
    constexpr Range intersectWith(Range other) const { return { std::max(minimum, other.minimum), std::min(maximum, other.maximum) }; }
    constexpr Range widenedFrom(Range previous) const;
    friend constexpr bool operator==(Range, Range) = default;
};

constexpr int64_t widenedMaximum(int64_t maximum)
{
    // The largest length is 2 ** 53 - 1: a counter below a length can still take a step from there.
    for (int64_t landmark : { int64_t { 0 }, int64_t { INT32_MAX }, int64_t { UINT32_MAX }, maxExactInteger - 1, maxExactInteger }) {
        if (maximum <= landmark)
            return landmark;
    }
    return maxTrackedInteger;
}

constexpr int64_t widenedMinimum(int64_t minimum)
{
    for (int64_t landmark : { int64_t { 0 }, int64_t { INT32_MIN }, -maxExactInteger }) {
        if (minimum >= landmark)
            return landmark;
    }
    return -maxTrackedInteger;
}

constexpr Range Range::widenedFrom(Range previous) const
{
    if (isEmpty() || previous.isEmpty())
        return *this;
    Range result = *this;
    if (minimum < previous.minimum)
        result.minimum = widenedMinimum(minimum);
    if (maximum > previous.maximum)
        result.maximum = widenedMaximum(maximum);
    return result;
}

static_assert(Primitive::Defined == (1u << numberOfEnumerators<PrimitiveSet>) - 1);
static_assert(!(Primitive::All & (Primitive::Absent | Primitive::LiteralString | Primitive::LiteralSymbol)));
static_assert(static_cast<double>(maxTrackedInteger + 1) == 0x1p62);
static_assert(Range::full().contains(maxTrackedInteger) && !Range::full().contains(maxTrackedInteger + 1));
static_assert(Range::empty().isSubsetOf(Range::one(0)));
static_assert(primitiveOfNumber(1) == PrimitiveSet::Integer);
static_assert(primitiveOfNumber(-0.0) == PrimitiveSet::WholeNumber);
static_assert(primitiveOfNumber(0x1p62) == PrimitiveSet::WholeNumber);
static_assert(primitiveOfNumber(0.5) == PrimitiveSet::FractionalNumber);
static_assert(Range { 0, 5 }.widenedFrom({ 0, 4 }) == Range::length());
static_assert(Range { -1, 4 }.widenedFrom({ 0, 4 }) == Range { INT32_MIN, 4 });

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
