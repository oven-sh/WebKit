/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMethodTableEntrypoints.h"
#include <optional>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

struct PlacedSelector {
    MethodSelector selector;
    // The entry of a shape is at offset - shape. The word at offsetOfShapeWord is 0 - shape, so compiled code adds it as it is.
    uint32_t offset { 0 };
    FunctionID lookup;
};

// While the bit of the guard is set, the entry holds what the image has there. While it is clear, the entry holds the lookup of the selector.
struct MethodTableDependent {
    friend auto operator<=>(const MethodTableDependent&, const MethodTableDependent&) = default;

    GuardBitID guard;
    uint32_t entry { 0 };
    uint32_t selector { 0 };
};

// What an object finds under the name of a method, by the shape of the object.
struct MethodTable {
    JS_EXPORT_PRIVATE std::optional<uint32_t> offsetOf(MethodSelector) const;
    size_t sizeInBytes() const { return entries.size() * sizeof(uint32_t); }

    // In the order of the selectors.
    Vector<PlacedSelector> selectors;
    Vector<FunctionID> entries;
    Vector<MethodTableDependent> dependents;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
