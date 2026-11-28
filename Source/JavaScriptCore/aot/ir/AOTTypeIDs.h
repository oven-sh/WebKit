/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTPrimitives.h"
#include <array>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

using ObjectID = ID<struct ObjectIDTag>;
using ObjectSetID = ID<struct ObjectSetIDTag>;
using StringSetID = ID<struct StringSetIDTag>;
using KeySetID = ID<struct KeySetIDTag>;
using ClassPartsID = ID<struct ClassPartsIDTag>;

enum class IDHome : uint8_t {
    Value = 0,
    SmallRange = 1,
    Program = 2,
    Thread = 3,
};

inline constexpr uint32_t idHomeShift = 30;
inline constexpr uint32_t idPositionMask = (1u << idHomeShift) - 1;
inline constexpr uint32_t largestIDPosition = idPositionMask - 2;

template<typename Tag> constexpr IDHome homeOf(ID<Tag> id) { return static_cast<IDHome>(id.bits() >> idHomeShift); }
template<typename Tag> constexpr uint32_t positionOf(ID<Tag> id) { return id.bits() & idPositionMask; }
template<typename Tag> constexpr bool liesInThread(ID<Tag> id) { return id && homeOf(id) == IDHome::Thread; }
template<typename Tag> constexpr bool liesInProgram(ID<Tag> id) { return homeOf(id) == IDHome::Program; }
template<typename Tag> constexpr bool isItsOwnValue(ID<Tag> id) { return !(id.bits() >> (idHomeShift + 1)); }

template<typename IDType>
constexpr IDType idAt(IDHome home, uint32_t position)
{
    RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(position <= (home == IDHome::Thread ? largestIDPosition : idPositionMask));
    return IDType::fromBits(static_cast<uint32_t>(home) << idHomeShift | position);
}

template<typename SetID, typename MemberID>
constexpr SetID setOfOne(MemberID member)
{
    return idAt<SetID>(IDHome::Value, member.index());
}
template<typename Tag> constexpr bool holdsOneMember(ID<Tag> set) { return homeOf(set) == IDHome::Value; }

inline constexpr std::array<int64_t, 4> minimumLandmarks { 0, INT32_MIN, -maxExactInteger, -maxTrackedInteger };
inline constexpr std::array<int64_t, 7> maximumLandmarks { 0, INT32_MAX, UINT32_MAX, maxTypedArrayLength, maxExactInteger - 1, maxExactInteger, maxTrackedInteger };
inline constexpr uint32_t minimumLandmarkShift = 14;
inline constexpr uint32_t maximumLandmarkShift = 16;
inline constexpr uint32_t smallBoundBits = 15;
inline constexpr int64_t largestSmallBound = (1 << (smallBoundBits - 1)) - 1;
inline constexpr int64_t smallestSmallBound = -(1 << (smallBoundBits - 1));
inline constexpr uint16_t primitivesWithLists = Primitive::LiteralString | Primitive::LiteralSymbol;

static_assert(numberOfEnumerators<PrimitiveSet> <= minimumLandmarkShift);

template<size_t size>
constexpr std::optional<uint32_t> landmarkIndexOf(const std::array<int64_t, size>& landmarks, int64_t bound)
{
    for (uint32_t index = 0; index < size; ++index) {
        if (landmarks[index] == bound)
            return index;
    }
    return std::nullopt;
}

constexpr std::optional<TypeID> tryMakeTypeOfPrimitives(uint16_t primitives, Range integers)
{
    if (primitives & primitivesWithLists)
        return std::nullopt;
    if (!(primitives & Primitive::Integer))
        return TypeID::fromBits(primitives);
    if (primitives == Primitive::Integer && integers.minimum >= smallestSmallBound && integers.maximum <= largestSmallBound) {
        uint32_t mask = (1u << smallBoundBits) - 1;
        uint32_t bounds = (static_cast<uint32_t>(integers.minimum) & mask) | (static_cast<uint32_t>(integers.maximum) & mask) << smallBoundBits;
        return TypeID::fromBits(static_cast<uint32_t>(IDHome::SmallRange) << idHomeShift | bounds);
    }
    if (integers == Range::full()) {
        static_assert(minimumLandmarks.back() == Range::full().minimum && maximumLandmarks.back() == Range::full().maximum);
        return TypeID::fromBits(primitives | static_cast<uint32_t>(minimumLandmarks.size() - 1) << minimumLandmarkShift | static_cast<uint32_t>(maximumLandmarks.size() - 1) << maximumLandmarkShift);
    }
    auto minimum = landmarkIndexOf(minimumLandmarks, integers.minimum);
    if (!minimum)
        return std::nullopt;
    auto maximum = landmarkIndexOf(maximumLandmarks, integers.maximum);
    if (!maximum)
        return std::nullopt;
    return TypeID::fromBits(primitives | *minimum << minimumLandmarkShift | *maximum << maximumLandmarkShift);
}

constexpr TypeID typeOfPrimitives(uint16_t primitives)
{
    return *tryMakeTypeOfPrimitives(primitives, primitives & Primitive::Integer ? Range::full() : Range::empty());
}

constexpr bool holdsOnlyPrimitives(TypeID type) { return isItsOwnValue(type); }

constexpr uint16_t primitivesInID(TypeID type)
{
    return homeOf(type) == IDHome::SmallRange ? Primitive::Integer : static_cast<uint16_t>(type.bits() & Primitive::Defined);
}

ALWAYS_INLINE constexpr Range integersInID(TypeID type)
{
    auto signedBound = [](uint32_t bits) {
        return static_cast<int64_t>(static_cast<int32_t>(bits << (32 - smallBoundBits)) >> (32 - smallBoundBits));
    };
    if (homeOf(type) == IDHome::SmallRange)
        return { signedBound(type.bits()), signedBound(type.bits() >> smallBoundBits) };
    if (!(type.bits() & Primitive::Integer))
        return Range::empty();
    return { minimumLandmarks[type.bits() >> minimumLandmarkShift & 3], maximumLandmarks[type.bits() >> maximumLandmarkShift & 7] };
}

constexpr bool hasOnlyPrimitivesIn(TypeID type, uint16_t allowed) { return holdsOnlyPrimitives(type) && !(primitivesInID(type) & ~allowed); }
constexpr bool isNeverType(TypeID type) { return !type.bits(); }
constexpr bool isIntegerType(TypeID type) { return !isNeverType(type) && hasOnlyPrimitivesIn(type, Primitive::Integer); }
constexpr bool isInt32Type(TypeID type) { return isIntegerType(type) && integersInID(type).isSubsetOf(Range::int32()); }
constexpr bool isNumberType(TypeID type) { return !isNeverType(type) && hasOnlyPrimitivesIn(type, Primitive::Number); }
constexpr bool isBooleanType(TypeID type) { return !isNeverType(type) && hasOnlyPrimitivesIn(type, Primitive::Boolean); }
constexpr bool isNullishType(TypeID type) { return !isNeverType(type) && hasOnlyPrimitivesIn(type, Primitive::UndefinedOrNull); }

static_assert(integersInID(*tryMakeTypeOfPrimitives(Primitive::Integer, { -1, 255 })) == Range { -1, 255 });
static_assert(integersInID(*tryMakeTypeOfPrimitives(Primitive::Integer, Range::one(-1))) == Range::one(-1));
static_assert(integersInID(*tryMakeTypeOfPrimitives(Primitive::Integer, { -2, -1 })) == Range { -2, -1 });
static_assert(integersInID(*tryMakeTypeOfPrimitives(Primitive::Integer, Range::int32())) == Range::int32());
static_assert(integersInID(*tryMakeTypeOfPrimitives(Primitive::Integer | Primitive::Undefined, Range::length())) == Range::length());
static_assert(!tryMakeTypeOfPrimitives(Primitive::Integer, { 0, 100000 }));
static_assert(isNeverType(typeOfPrimitives(0)));
static_assert(isInt32Type(*tryMakeTypeOfPrimitives(Primitive::Integer, Range::one(7))));

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
