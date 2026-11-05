/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTSchemaEnums.h"
#include <compare>

namespace JSC { namespace AOT { namespace Typed {

inline constexpr unsigned numberOfCodeRolesOfOneFunction = 8;
static_assert(sizeof(CodeRole) == 1);
static_assert(static_cast<unsigned>(CodeRole::TableEntry) == numberOfCodeRolesOfOneFunction);
static_assert(static_cast<unsigned>(CodeRole::Serializer) + 1 == numberOfEnumerators<CodeRole>);

inline constexpr uint16_t coroutineResumeEntry = 1;

constexpr bool isTableRole(CodeRole role)
{
    return role == CodeRole::TableEntry || role == CodeRole::TableLookup;
}

constexpr bool isGenericEntry(CodeRole role)
{
    return role == CodeRole::GenericCallEntry || role == CodeRole::GenericConstructEntry || role == CodeRole::AllocatingConstructEntry;
}

constexpr bool walksInputBody(CodeRole role)
{
    return role == CodeRole::Body || role == CodeRole::SlowBody || role == CodeRole::CoroutineBody;
}

constexpr bool namesOnlyUnitsOfItsFunction(CodeRole role)
{
    return role == CodeRole::GenericCallEntry || isTableRole(role);
}

constexpr bool returnsResultOfFunction(CodeRole role)
{
    return role == CodeRole::Body || role == CodeRole::SlowBody || role == CodeRole::ParameterGuard;
}

constexpr bool isBodyOfCoroutine(CodeRole role, bool)
{
    return role == CodeRole::CoroutineBody;
}

struct CodeUnit {
    constexpr CodeUnit() = default;
    constexpr CodeUnit(FunctionID function, CodeRole role, uint16_t numberOfBoundArguments = 0)
        : function(function)
        , role(role)
        , numberOfBoundArguments(numberOfBoundArguments)
    {
    }

    constexpr explicit operator bool() const { return !!function; }
    friend constexpr bool operator==(const CodeUnit&, const CodeUnit&) = default;
    friend constexpr auto operator<=>(const CodeUnit&, const CodeUnit&) = default;

    static constexpr uint32_t numberOfLayoutsThatSerializersCanName = 1u << 24;
    static constexpr CodeUnit serializer(FunctionID listedUnder, LayoutID layout)
    {
        CodeUnit unit(listedUnder, CodeRole::Serializer, static_cast<uint16_t>(layout.index()));
        unit.upperBitsOfLayout = static_cast<uint8_t>(layout.index() >> 16);
        return unit;
    }
    constexpr LayoutID layoutOfSerializer() const { return LayoutID(static_cast<uint32_t>(upperBitsOfLayout) << 16 | numberOfBoundArguments); }

    FunctionID function;
    CodeRole role { CodeRole::Body };
    uint8_t upperBitsOfLayout { 0 };
    uint16_t numberOfBoundArguments { 0 };
};
static_assert(sizeof(CodeUnit) == 8);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
