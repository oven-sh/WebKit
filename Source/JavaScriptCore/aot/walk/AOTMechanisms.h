/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

enum class Mechanism : uint16_t {
    TypedOperations = 1 << 0,
    Guesses = 1 << 1,
    Arrivals = 1 << 2,
    DirectCalls = 1 << 3,
    Inlining = 1 << 4,
    UnboxedNumbers = 1 << 5,
    EscapeAnalysis = 1 << 6,
    LoopInvariants = 1 << 7,
    Folding = 1 << 8,
    TypedSlots = 1 << 9,
    Handlers = 1 << 10,
    BuiltinCalls = 1 << 11,
    ParameterGuards = 1 << 12,
    Deferral = 1 << 13,
    CellsInFrames = 1 << 14,
    PromotedEnvironments = 1 << 15,
};
using Mechanisms = OptionSet<Mechanism>;

inline constexpr Mechanisms allMechanisms {
    Mechanism::TypedOperations, Mechanism::Guesses, Mechanism::Arrivals, Mechanism::DirectCalls, Mechanism::Inlining, Mechanism::UnboxedNumbers,
    Mechanism::EscapeAnalysis, Mechanism::LoopInvariants, Mechanism::Folding, Mechanism::TypedSlots, Mechanism::Handlers, Mechanism::BuiltinCalls,
    Mechanism::ParameterGuards, Mechanism::Deferral, Mechanism::CellsInFrames, Mechanism::PromotedEnvironments
};

constexpr Mechanisms dependenciesOf(Mechanism mechanism)
{
    switch (mechanism) {
    case Mechanism::Guesses:
    case Mechanism::UnboxedNumbers:
    case Mechanism::ParameterGuards:
        return { Mechanism::TypedOperations };
    case Mechanism::EscapeAnalysis:
        return { Mechanism::TypedOperations, Mechanism::Folding };
    case Mechanism::CellsInFrames:
        return { Mechanism::EscapeAnalysis };
    case Mechanism::LoopInvariants:
        return { Mechanism::TypedOperations, Mechanism::Guesses, Mechanism::Folding };
    case Mechanism::Arrivals:
    case Mechanism::Inlining:
    case Mechanism::BuiltinCalls:
        return { Mechanism::DirectCalls };
    case Mechanism::TypedSlots:
        return { Mechanism::TypedOperations, Mechanism::UnboxedNumbers };
    case Mechanism::TypedOperations:
    case Mechanism::DirectCalls:
    case Mechanism::Folding:
    case Mechanism::Handlers:
    case Mechanism::Deferral:
    case Mechanism::PromotedEnvironments:
        return { };
    }
    return { };
}

constexpr Mechanisms withoutMechanismsMissingDependencies(Mechanisms mechanisms)
{
    Mechanisms result;
    for (Mechanism mechanism : mechanisms) {
        if (mechanisms.containsAll(dependenciesOf(mechanism)))
            result.add(mechanism);
    }
    return result == mechanisms ? result : withoutMechanismsMissingDependencies(result);
}

enum class WalkMode : uint8_t { AsBuilt, NothingCarried, Closed };

constexpr Mechanisms mechanismsOfWay(WalkMode way, Mechanisms ofBuild)
{
    switch (way) {
    case WalkMode::AsBuilt:
        return ofBuild;
    case WalkMode::NothingCarried:
        return withoutMechanismsMissingDependencies(ofBuild - Mechanisms { Mechanism::LoopInvariants, Mechanism::EscapeAnalysis, Mechanism::Inlining, Mechanism::Handlers });
    case WalkMode::Closed:
        return ofBuild & Mechanisms { Mechanism::Folding, Mechanism::DirectCalls, Mechanism::Arrivals };
    }
    return ofBuild;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
