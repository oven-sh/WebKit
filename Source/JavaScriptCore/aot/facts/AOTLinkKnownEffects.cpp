/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinkKnownEffects.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include <algorithm>
#include <tuple>

namespace JSC { namespace AOT { namespace Typed {

// new.target may select a class below the one that an allocation names, and a store through a base class lands in an object of any class below it. So the whole row answers to one bit.
uint64_t bitOfLayoutInKnownEffects(const Program& program, LayoutID layout)
{
    // `class A extends B {}` beside `class B extends A {}` compiles.
    size_t stepsLeft = program.layouts.size();
    LayoutID top = layout;
    for (LayoutID above = writtenBaseOf(program.at(top)); above; above = writtenBaseOf(program.at(top))) {
        if (!stepsLeft--)
            return objectsOfAnyLayout;
        top = above;
    }
    std::optional<uint8_t> bit = program.locationBitOf(top);
    return bit ? uint64_t { 1 } << (*bit & 63) : objectsOfAnyLayout;
}

uint64_t LinkKnownEffects::layoutsWrittenBy(FunctionID function) const
{
    return function.index() < layoutsWrittenByFunction.size() ? layoutsWrittenByFunction[function.index()] : objectsOfAnyLayout;
}

const LinkedEffectsGivenArguments* LinkKnownEffects::findEffectsGivenArguments(FunctionID function) const
{
    auto found = std::ranges::lower_bound(givenArguments, function, { }, &LinkedEffectsGivenArguments::function);
    return found != givenArguments.end() && found->function == function ? &*found : nullptr;
}

const LinkedCallOfSeveral* LinkKnownEffects::findCallOfSeveral(FunctionID caller, ValueID site) const
{
    auto placeOf = [](const LinkedCallOfSeveral& call) {
        return std::tuple { call.caller, call.site };
    };
    auto found = std::ranges::lower_bound(callsOfSeveral, std::tuple { caller, site }, { }, placeOf);
    return found != callsOfSeveral.end() && found->caller == caller && found->site == site ? &*found : nullptr;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
