/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLayoutData.h"
#include "PropertyOffset.h"
#include "StructureID.h"
#include <memory>
#include <span>
#include <wtf/Vector.h>
#include <wtf/text/Latin1Character.h>

namespace JSC { namespace AOT {

struct PropertyListEntry {
    UniquedStringImpl* name;
    const FieldData* field;
    PropertyOffset offsetInExtraProperties;
};

struct FieldAsProperty {
    // Copies of a key's text take whole blocks: that many bytes can be read from any byte of the text.
    static constexpr size_t jsonKeyBlockSize = 16;

    UniquedStringImpl* name;
    const FieldData* field;
    // ,"name": as JSON writes it. Empty for a symbol and for a name with a character outside Latin-1.
    std::span<const Latin1Character> jsonKey;
    // For a writer of JSON that reads the slot by itself. What it cannot write so has the representation of a deferred closure here: a field out of line, a field without a key for JSON.
    uint32_t offsetForJSON;
    SlotRepresentation representationForJSON;
};

// The keys of one list lie one behind the other.
inline size_t jsonKeysSize(std::span<const FieldAsProperty> list)
{
    return list.empty() ? 0 : std::to_address(list.back().jsonKey.end()) - list.front().jsonKey.data();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
