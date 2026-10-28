/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTRepresentation.h"
#include "AOTVector.h"
#include "Reg.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

enum class EntryConvention : uint8_t { Direct, GenericRegisters, GenericBuffer };

struct ArgumentLocation {
    enum class Kind : uint8_t { None, GPR, FPR, Stack };

    friend bool operator==(const ArgumentLocation&, const ArgumentLocation&) = default;

    Kind kind { Kind::None };
    Reg reg;
    uint32_t stackIndex { 0 };
    Representation representation { Representation::Void };
};

struct ArgumentLocations {
    uint32_t numberOfStackWords() const { return std::max(numberOfStackArguments, numberOfStackResults); }

    EntryConvention convention { EntryConvention::Direct };
    Vector<ArgumentLocation, 8> parameters;
    Vector<ArgumentLocation, 2> results;
    uint32_t numberOfStackArguments { 0 };
    uint32_t numberOfStackResults { 0 };
};

struct ConventionRegisters {
    unsigned numberOfGPRs { 0 };
    unsigned numberOfFPRs { 0 };
};

constexpr bool isPassedInFPR(Representation representation)
{
    RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(representation != Representation::Unchosen && representation != Representation::Void);
    return isPassedInFPROf(representation);
}

EntryConvention entryConventionOf(const Function&);

constexpr bool isCellOrBoxedValue(Representation representation)
{
    return representation == Representation::Cell || representation == Representation::JSValue;
}

constexpr bool isDoubleOrMarker(Representation representation)
{
    return representation == Representation::Double || representation == Representation::OptionalDouble;
}

constexpr bool isInt64OrMarker(Representation representation)
{
    return representation == Representation::Int64 || representation == Representation::OptionalInt64;
}

// What a tail call returns is proven to be inside the result type of its caller, so where the caller returns a cell a boxed result holds one, and a boxed cell is the pointer.
// A number has the same bits where undefined or null may come in its place.
constexpr bool haveSameRegisterRepresentation(Representation returned, Representation expected)
{
    return returned == expected
        || (isCellOrBoxedValue(returned) && isCellOrBoxedValue(expected))
        || (isDoubleOrMarker(returned) && isDoubleOrMarker(expected))
        || (isInt64OrMarker(returned) && isInt64OrMarker(expected));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
