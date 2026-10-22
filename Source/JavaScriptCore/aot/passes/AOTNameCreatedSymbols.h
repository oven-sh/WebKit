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
// None unless the operation is a call at a position of a module, with no argument or with one constant string.
std::optional<PropertyKey> keyOfSymbolsCreatedBy(const Function&, const Value&);

// The key of the symbol that a call of Symbol.for itself answers: one symbol for the text, wherever the call stands and however often it runs.
// None unless the callee is that built-in by binding and the first argument is a constant string.
std::optional<PropertyKey> keyOfSymbolRegisteredBy(const BuiltinTable&, const Function&, const Value&);

// Interns the key of every call of Symbol itself that runs at most once each time its function runs, of every call of Symbol.for itself with a constant text,
// and of every constant text that a call hands to a built-in, by binding, at the place where the table says the built-in takes the key of a property. No body changes.
// The flow analysis takes the program as it is, so a key that it is to answer such a call with has to be there before it starts.
void nameCreatedSymbols(Program&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
