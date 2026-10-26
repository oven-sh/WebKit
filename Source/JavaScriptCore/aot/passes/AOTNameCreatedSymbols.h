/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include <optional>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

// The callee is Symbol itself, by binding: nothing stands between the built-in and the call but upcasts.
bool callsSymbolDirectly(const BuiltinTable&, const Function&, const Value&);

// The key that stands for the symbols a call makes if it enters Symbol: their description, and where the call stands in the source.
std::optional<PropertyKey> keyOfSymbolsCreatedBy(const Function&, const Value&);

// The key of the symbol that a call of Symbol.for itself answers: one symbol for the text, wherever the call stands and however often it runs.
std::optional<PropertyKey> keyOfSymbolRegisteredBy(const BuiltinTable&, const Function&, const Value&);

void nameCreatedSymbols(Program&, BuildContext&);

// The key alone tells the name that a function takes from it: its text, or the description of the symbol in brackets.
constexpr bool tellsNameOfFunction(const PropertyKey& key)
{
    switch (key.kind) {
    case PropertyKeyKind::Text:
    case PropertyKeyKind::RegisteredSymbol:
    case PropertyKeyKind::CreatedSymbol:
        return !!key.text;
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::ModulePrivateName:
    case PropertyKeyKind::PrivateName:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

// The symbols that a module of the library creates, holds in one variable that is stored to once and is not exported, and uses only as keys of accesses. By the index of their keys.
BitVector findLibrarySymbolsUsedOnlyAsKeys(const Program&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
