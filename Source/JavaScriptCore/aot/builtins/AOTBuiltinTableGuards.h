/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinFeatures.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string_view>

namespace JSC {

class CallFrame;
class PropertyName;

namespace AOT {

using HostFunctionEntryPoint = EncodedJSValue (JSC_HOST_CALL_ATTRIBUTES *)(JSGlobalObject*, CallFrame*);

struct GuardedEngineSymbol {
    std::string_view symbol;
    RemovableFeature feature;
    RemovedFeatureStub stub;
    uint32_t positionOfBuiltin;
};

inline constexpr GuardedEngineSymbol guardedEngineSymbols[] = {
#define AOT_GUARDED_ENGINE_SYMBOL(symbol, feature, stub, positionOfBuiltin) { #symbol, RemovableFeature::feature, RemovedFeatureStub::stub, positionOfBuiltin },
    FOR_EACH_GUARDED_ENGINE_SYMBOL(AOT_GUARDED_ENGINE_SYMBOL)
#undef AOT_GUARDED_ENGINE_SYMBOL
};

static_assert(std::ranges::is_sorted(guardedEngineSymbols, { }, &GuardedEngineSymbol::symbol));

consteval const GuardedEngineSymbol* findGuardedEngineSymbol(std::string_view symbol)
{
    auto found = std::ranges::lower_bound(guardedEngineSymbols, symbol, { }, &GuardedEngineSymbol::symbol);
    if (found == std::ranges::end(guardedEngineSymbols) || found->symbol != symbol)
        return nullptr;
    return found;
}

template<size_t size>
struct SymbolText {
    consteval SymbolText(const char (&text)[size])
    {
        std::copy_n(text, size, characters);
    }

    consteval std::string_view view() const { return { characters, size - 1 }; }

    char characters[size] { };
};

template<auto entryPoint, SymbolText text>
ALWAYS_INLINE constexpr auto registeredEntryPoint()
{
    constexpr const GuardedEngineSymbol* guarded = findGuardedEngineSymbol(text.view());
    if constexpr (!!guarded) {
        static_assert(guarded->stub == RemovedFeatureStub::Throw, "the entry point of a removed built-in throws");
        if !consteval {
            recordRemovableEntryPoint<guarded->feature, guarded->stub, entryPoint, GuardOrigin::BuiltinNamedByEngine, guarded->positionOfBuiltin>();
        }
    }
    return entryPoint;
}

// The parameter's type picks the entry point out of the overloads that share its name.
template<HostFunctionEntryPoint entryPoint, SymbolText text>
ALWAYS_INLINE constexpr HostFunctionEntryPoint registeredHostFunction()
{
    return registeredEntryPoint<entryPoint, text>();
}

} } // namespace JSC::AOT

#define JSC_REGISTERED_HOST_FUNCTION(cppName) (::JSC::AOT::registeredHostFunction<cppName, #cppName>())

#else

#define JSC_REGISTERED_HOST_FUNCTION(cppName) cppName

#endif // ENABLE(AOT)
