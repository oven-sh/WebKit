/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSchemaEnums.h"
#include "AOTVector.h"
#include "JSExportMacros.h"
#include "Reg.h"
#include <algorithm>
#include <span>

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

constexpr bool isPassedInFPR(Representation representation)
{
    RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(representation != Representation::Unchosen && representation != Representation::Void);
    return isPassedInFPROf(representation);
}

constexpr bool isPassedAs(Representation handed, Representation passed)
{
    return handed == passed;
}

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

ALWAYS_INLINE constexpr bool haveSameRegisterRepresentation(Representation returned, Representation expected)
{
    return returned == expected
        || (isCellOrBoxedValue(returned) && isCellOrBoxedValue(expected))
        || (isDoubleOrMarker(returned) && isDoubleOrMarker(expected))
        || (isInt64OrMarker(returned) && isInt64OrMarker(expected));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
