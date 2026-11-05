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

    static constexpr CodeUnit ofMethodTable(FunctionID method, CodeRole role, uint32_t placeOfSelector)
    {
        CodeUnit unit(method, role, static_cast<uint16_t>(placeOfSelector));
        unit.highByteOfPlaceOfSelector = static_cast<uint8_t>(placeOfSelector >> 16);
        return unit;
    }

    constexpr explicit operator bool() const { return !!function; }
    constexpr uint32_t placeOfSelector() const { return static_cast<uint32_t>(highByteOfPlaceOfSelector) << 16 | numberOfBoundArguments; }
    friend constexpr bool operator==(const CodeUnit&, const CodeUnit&) = default;
    friend constexpr auto operator<=>(const CodeUnit&, const CodeUnit&) = default;

    FunctionID function;
    CodeRole role { CodeRole::Body };
    uint8_t highByteOfPlaceOfSelector { 0 };
    uint16_t numberOfBoundArguments { 0 };
};
static_assert(sizeof(CodeUnit) == 8);
static_assert(CodeUnit::ofMethodTable(FunctionID(), CodeRole::TableLookup, 0xabcdef).placeOfSelector() == 0xabcdef);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
