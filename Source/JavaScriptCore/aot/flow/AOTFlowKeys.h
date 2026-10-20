/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowIDs.h"
#include "AOTPrimitives.h"
#include "AOTSortedSet.h"
#include <optional>
#include <wtf/Forward.h>

namespace JSC { namespace AOT { namespace Typed {

class Program;

inline constexpr Range arrayIndexRange { 0, 4294967294ll };

struct FlowKeySet {
    SortedSet<PropertyKeyID, 2> names;
    Range indexes { Range::empty() };
    bool anyNumericName { false };
    bool anyString { false };
    bool anySymbol { false };

    static FlowKeySet none() { return { }; }

    static FlowKeySet name(PropertyKeyID key)
    {
        FlowKeySet keys;
        keys.names.add(key);
        return keys;
    }

    static FlowKeySet index(Range range)
    {
        FlowKeySet keys;
        if (Range clamped = range.intersectWith(arrayIndexRange); !clamped.isEmpty())
            keys.indexes = clamped;
        return keys;
    }

    // The string of a number is an array index or a canonical numeric string. A value under such a name is held with the indexed values.
    static FlowKeySet everyNumber()
    {
        FlowKeySet keys;
        keys.indexes = arrayIndexRange;
        keys.anyNumericName = true;
        return keys;
    }

    static FlowKeySet everyString()
    {
        FlowKeySet keys = everyNumber();
        keys.anyString = true;
        return keys;
    }

    static FlowKeySet everyKey()
    {
        FlowKeySet keys = everyString();
        keys.anySymbol = true;
        return keys;
    }

    bool isEmpty() const { return names.isEmpty() && indexes.isEmpty() && !hasUnlistedNames(); }
    bool hasOnlyNames() const { return indexes.isEmpty() && !hasUnlistedNames(); }
    bool hasUnnamedKeys() const { return anyString || anySymbol; }
    bool hasUnlistedNames() const { return anyNumericName || hasUnnamedKeys(); }
    PropertyKeyID onlyName() const { return hasOnlyNames() && names.size() == 1 ? names[0] : PropertyKeyID(); }

    bool unite(const FlowKeySet&);
    bool isSubsetOf(const FlowKeySet&, const Program&) const;
    bool mayName(PropertyKeyID, const Program&) const;
    bool mayNameIndex() const { return !indexes.isEmpty(); }

    void dump(PrintStream&, const Program&) const;

    friend bool operator==(const FlowKeySet&, const FlowKeySet&) = default;
};

enum class FlowKeyClass : uint8_t {
    String,
    Index,
    Symbol,
    PrivateName,
};

FlowKeyClass classOf(const Program&, PropertyKeyID);
std::optional<uint32_t> arrayIndexOf(const Program&, PropertyKeyID);
FlowKeySet keySetOf(const Program&, PropertyKeyID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
