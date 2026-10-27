/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTSchemaIDs.h"
#include "AOTTypeKinds.h"
#include "AOTVector.h"
#include <optional>
#include <span>

namespace WTF {
class Hasher;
}

namespace JSC { namespace AOT { namespace Typed {

class IRPrinter;
class TextParser;

struct Callees {
    bool isEmpty() const { return functions.empty() && builtins.empty(); }

    std::span<const FunctionID> functions;
    std::span<const BuiltinID> builtins;
};

// What an operation that calls may enter, or the functions that a signature stands for.
// Complete: nothing else is entered. Open: flow saw these arrive beside what it did not see.
struct CalleeSet {
    CalleeSet() = default;

    // Whoever makes a complete set says that nothing else is entered.
    static CalleeSet complete(Vector<FunctionID>&& functions, Vector<BuiltinID>&& builtins) { return { WTF::move(functions), WTF::move(builtins), true }; }
    static CalleeSet open(Vector<FunctionID>&& functions, Vector<BuiltinID>&& builtins) { return { WTF::move(functions), WTF::move(builtins), false }; }

    // All that is entered.
    std::optional<ProvenValue<Callees>> all() const LIFETIME_BOUND
    {
        return isComplete ? std::optional(Proven::byClosedSet(written())) : std::nullopt;
    }
    // What has been seen to arrive. Anything else may arrive too, and what is listed may never arrive.
    Expected<Callees> seen() const LIFETIME_BOUND { return written(); }

    // The functions that flow has followed to the operation, whether or not anything else arrives.
    // Whoever gives something up for each of them, or counts them, has left out no function whose callers are all known (FunctionFlow::callers).
    ProvenValue<std::span<const FunctionID>> followed() const LIFETIME_BOUND { return Proven::byFlowOfCallees(functions.span()); }

    bool listsEveryCallee() const { return isComplete; }

    // For printers, tables, and whoever has to keep what a record names. It says nothing of a call.
    Callees written() const LIFETIME_BOUND { return { functions.span(), builtins.span() }; }

    friend bool operator==(const CalleeSet&, const CalleeSet&) = default;

private:
    friend void add(WTF::Hasher&, const CalleeSet&);
    friend void canonicalize(CalleeSet&);
    friend void printFields(IRPrinter&, const CalleeSet&);
    friend void parseFields(TextParser&, CalleeSet&);
    template<typename Subject, typename Visitor> friend void walkCalleeSet(Subject&, Visitor&);

    CalleeSet(Vector<FunctionID>&& functions, Vector<BuiltinID>&& builtins, bool isComplete)
        : functions(WTF::move(functions))
        , builtins(WTF::move(builtins))
        , isComplete(isComplete)
    {
    }

    Vector<FunctionID> functions;
    Vector<BuiltinID> builtins;
    bool isComplete { false };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
