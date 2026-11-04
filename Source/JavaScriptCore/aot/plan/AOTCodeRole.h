/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTSchemaEnums.h"
#include <compare>
#include <wtf/Hasher.h>

namespace JSC { namespace AOT { namespace Typed {

// CodeRole: one piece of machine code that Compile makes of a function of the program. The enum is the schema's (ir/schema/operations.def), because CallDirect names the role of the unit that it enters.
//   Body                         the one body of the function. Of a coroutine: the ramp, which makes the driver and starts the CoroutineBody
//                                Of a function with a ParameterGuard: the fast version. What the guard tests is proven at its entry: CallingConventions::typeBehindGuard()
//   GenericCallEntry, GenericConstructEntry, AllocatingConstructEntry
//                                entered by whoever holds a closure. Each goes on to CallingConventions::genericEntry().target
//   CoroutineBody                the body of a coroutine as written. It takes the parameters of the function and the driver behind them
//   ModuleStart                  of the body of a module with a state. It has no code: the call is one of the stub StartModule with the number of the start
//   SlowBody                     the slow version: the closed body with Folding and direct calls, built for size. Whoever has seen a test of the guard fail enters it, at its start: the guard, and a caller that has the tests in front of a copy of the fast version
//   ParameterGuard               entered by whoever has not proven what the fast version takes for granted. It tests the parameters once, falls into the Body where all pass, and jumps to the SlowBody where one fails
//   BoundFunction, GenericEntryOfBoundFunction
//   TableEntry, TableLookup      what the method table holds
//   Serializer                   of a layout: writes an object of the layout as JSON into a string builder, so that an object may hold objects of its own layout to any depth. It has no input body.
//                                Its function only says where it is listed: CallingConventions::serializerOf()
inline constexpr unsigned numberOfCodeRolesOfOneFunction = 8;
inline constexpr unsigned numberOfBoundRoles = 2;
static_assert(sizeof(CodeRole) == 1);
static_assert(static_cast<unsigned>(CodeRole::BoundFunction) == numberOfCodeRolesOfOneFunction);
static_assert(static_cast<unsigned>(CodeRole::TableEntry) == numberOfCodeRolesOfOneFunction + numberOfBoundRoles);
static_assert(static_cast<unsigned>(CodeRole::Serializer) + 1 == numberOfEnumerators<CodeRole>);

// Of the body of a generator that keeps what lives across a yield in a heap frame: it takes the frame, the resume mode and the value sent, and goes on behind the yield that the frame names.
inline constexpr uint16_t coroutineResumeEntry = 1;

constexpr bool isBoundRole(CodeRole role)
{
    return role == CodeRole::BoundFunction || role == CodeRole::GenericEntryOfBoundFunction;
}

constexpr bool isTableRole(CodeRole role)
{
    return role == CodeRole::TableEntry || role == CodeRole::TableLookup;
}

// Entered in the generic convention, by whoever holds a closure. The word of a closure names such a unit.
constexpr bool isGenericEntry(CodeRole role)
{
    return role == CodeRole::GenericCallEntry || role == CodeRole::GenericConstructEntry || role == CodeRole::AllocatingConstructEntry || role == CodeRole::GenericEntryOfBoundFunction;
}

// The walk makes it of the function's input body. Every other unit is a few operations in front of one of these.
constexpr bool walksInputBody(CodeRole role)
{
    return role == CodeRole::Body || role == CodeRole::SlowBody || role == CodeRole::CoroutineBody;
}

// It moves what it is handed and goes on to another unit of its function, or looks a name up as it is written. It tests for no type and allocates no object of the program, so its code names no other function.
// A guard tests for types, which may be the closures of a function. An entry for constructions allocates, and a slot of the layout may name the function that fills it later.
constexpr bool namesOnlyUnitsOfItsFunction(CodeRole role)
{
    return role == CodeRole::GenericCallEntry || isBoundRole(role) || isTableRole(role);
}

// What a direct call of the function enters, and what that goes on to. It takes the entry parameters of the function and returns what the function returns.
constexpr bool returnsResultOfFunction(CodeRole role)
{
    return role == CodeRole::Body || role == CodeRole::SlowBody || role == CodeRole::ParameterGuard;
}

// A suspension saves its frame, and it returns, throws and is listed in a trace as the body of the coroutine. It takes the driver behind the parameters of the function.
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

    // The number of the layout takes the 24 bits beside the function and the role.
    static constexpr uint32_t numberOfLayoutsThatSerializersCanName = 1u << 24;
    static constexpr CodeUnit serializer(FunctionID listedUnder, LayoutID layout)
    {
        CodeUnit unit(listedUnder, CodeRole::Serializer, static_cast<uint16_t>(layout.index()));
        unit.upperBitsOfLayout = static_cast<uint8_t>(layout.index() >> 16);
        return unit;
    }
    constexpr LayoutID layoutOfSerializer() const { return LayoutID(static_cast<uint32_t>(upperBitsOfLayout) << 16 | numberOfBoundArguments); }

    // Of a bound role: the target.
    FunctionID function;
    CodeRole role { CodeRole::Body };
    // No byte of the record is padding.
    // Zero but for a Serializer.
    uint8_t upperBitsOfLayout { 0 };
    // Zero but for the bound roles, and for the table roles, where it is the place of the selector in CallingConventions::methodSelectors().
    // Of a Serializer: the lower bits of the number of its layout.
    uint16_t numberOfBoundArguments { 0 };
};
static_assert(sizeof(CodeUnit) == 8);

inline void add(Hasher& hasher, const CodeUnit& unit)
{
    add(hasher, unit.function, static_cast<uint8_t>(unit.role), unit.upperBitsOfLayout, unit.numberOfBoundArguments);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
