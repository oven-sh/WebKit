/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTSchemaEnums.h"
#include "AOTVector.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

enum class FrameSlotKind : uint8_t { Value, Cell, NarrowReference };

constexpr std::optional<FrameSlotKind> frameSlotKindOf(Representation representation)
{
    switch (representation) {
    case Representation::JSValue:
        return FrameSlotKind::Value;
    case Representation::Cell:
        return FrameSlotKind::Cell;
    case Representation::Cell32:
        return FrameSlotKind::NarrowReference;
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

struct FrameMapRecords {
    Vector<uint32_t> words;
    bool didReportLimit { false };

    uint32_t addMap(std::span<const uint32_t> map);

private:
    Vector<uint32_t> m_offsets;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
