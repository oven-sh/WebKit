/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "JSExportMacros.h"
#include <compare>
#include <wtf/PrintStream.h>
#include <wtf/ScopedLambda.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

struct PlacementContext;
struct TableEncoder;

struct GlobalBindingKey {
    PropertyKeyID key;
    bool seesScriptBindings { false };
    bool isStore { false };
    bool isForTypeOf { false };

    friend auto operator<=>(const GlobalBindingKey&, const GlobalBindingKey&) = default;
};

struct PlacedGlobalBinding {
    GlobalBindingKey key;
    bool isStoredByName { false };
};

struct GlobalBindingTable {
    static constexpr uint32_t seesScriptBindingsFlag = 1u << 31;
    static constexpr uint32_t isStoredByNameFlag = 1u << 30;
    static constexpr uint32_t isForTypeOfFlag = 1u << 29;
    static constexpr uint32_t identifierMask = isForTypeOfFlag - 1;

    JS_EXPORT_PRIVATE void dump(PrintStream&, const Program&) const;

    Vector<PlacedGlobalBinding> bindings;
};

struct GlobalBindingSite;
// What collectGlobalBindingSites() gives for a function with a body.
using GlobalBindingSitesOfBodies = ScopedLambda<const Vector<GlobalBindingSite>&(FunctionID)>;
// The list takes the functions in which an operation was replaced by another because there are more bindings than the limit allows.
GlobalBindingTable placeGlobalBindings(ProgramEditor&, const PlacementContext&, TypeArena&, const GlobalBindingSitesOfBodies* = nullptr, Vector<FunctionID>* functionsWithOtherOperations = nullptr);
void encodeGlobalBindingTable(TableEncoder&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
