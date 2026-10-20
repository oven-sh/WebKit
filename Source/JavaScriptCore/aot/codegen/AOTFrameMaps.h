/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTSchemaEnums.h"
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Forward.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class FunctionAnalyses;
class Program;
struct Function;

enum class FrameSlotKind : uint8_t { Value, Cell, NarrowReference };

struct FrameRoot {
    ValueID value;
    FrameSlotKind kind;
};

struct FrameResultRoot {
    ValueID producer;
    uint32_t index;
    FrameSlotKind kind;
};

struct SuspensionRoots {
    ValueID suspension;
    Vector<FrameRoot, 8> roots;
    Vector<FrameResultRoot, 2> resultRoots;

    size_t size() const { return roots.size() + resultRoots.size(); }
};

class FrameMaps {
public:
    FrameMaps() = default;

    const SuspensionRoots& rootsAt(ValueID suspension) const LIFETIME_BOUND;
    unsigned numberOfSuspensions() const { return m_suspensions.size(); }
    BitVector suspensionValues() const;
    void dump(PrintStream&, const Function&) const;

private:
    friend FrameMaps computeFrameMaps(const Program&, const Function&, FunctionAnalyses&);

    Vector<SuspensionRoots> m_suspensions;
};

constexpr std::optional<FrameSlotKind> frameSlotKindOf(Representation representation)
{
    switch (representation) {
    case Representation::JSValue:
    case Representation::LiteralString:
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

FrameMaps computeFrameMaps(const Program&, const Function&, FunctionAnalyses&);

struct FrameMapRecords {
    Vector<uint32_t> words;

    uint32_t addMap(std::span<const uint32_t> map);

private:
    Vector<uint32_t> m_offsets;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
