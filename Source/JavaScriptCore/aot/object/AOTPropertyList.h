/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLayoutData.h"
#include "PropertyOffset.h"
#include <memory>
#include <span>
#include <wtf/text/Latin1Character.h>

namespace JSC { namespace AOT {

struct PropertyListEntry {
    UniquedStringImpl* name;
    const FieldData* field;
    PropertyOffset offsetInExtraProperties;
};

struct FieldAsProperty {
    static constexpr size_t jsonKeyBlockSize = 16;

    UniquedStringImpl* name;
    const FieldData* field;
    std::span<const Latin1Character> jsonKey;
    uint32_t offsetForJSON;
    SlotRepresentation representationForJSON;
};

inline size_t jsonKeysSize(std::span<const FieldAsProperty> list)
{
    return list.empty() ? 0 : std::to_address(list.back().jsonKey.end()) - list.front().jsonKey.data();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
