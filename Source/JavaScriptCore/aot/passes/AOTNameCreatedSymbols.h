/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include <optional>

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

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
