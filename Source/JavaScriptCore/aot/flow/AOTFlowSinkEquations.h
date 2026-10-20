/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOpcode.h"
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

inline constexpr OperandUses escapingUses {
    OperandUse::Retains,
    OperandUse::Returns,
    OperandUse::RetainsContents,
    OperandUse::PassesToUserCode,
};

// What becomes of the values an object holds does not move the object.
inline constexpr OperandUses frameLocalBlockers = escapingUses - OperandUses { OperandUse::RetainsContents };

inline constexpr OperandUses mutatingUses {
    OperandUse::WritesContents,
    OperandUse::MovesElements,
    OperandUse::StoresByKey,
    OperandUse::DeletesByKey,
    OperandUse::AssignsFrom,
    OperandUse::DefinesName,
    OperandUse::TakesDescriptor,
    OperandUse::ChangesIntegrity,
    OperandUse::ChangesPrototype,
    OperandUse::BecomesPrototype,
};

inline constexpr OperandUses nativeCodeUses = OperandUses::all() - mutatingUses;

enum class SinkTransform : uint8_t {
    Same,
    Argument,
    Leaves,
    IfReturned,
    Lifetime,
    Contents,
};

OperandUses applyTransform(SinkTransform, OperandUses);

// A dependency without a condition names its node as the condition too, so the solver walks both alike.
struct SinkDependency {
    uint32_t node { 0 };
    uint32_t condition { 0 };
    SinkTransform transform { SinkTransform::Same };
};

struct SinkEquations {
    Vector<OperandUses> base;
    Vector<OperandUses> sinks;
    // A pointer into a frame may be in no cell, however soon that cell dies, and native code takes cells of the heap only.
    // The second solution counts each store into a cell and each operand of native code as a use that retains.
    Vector<OperandUses> baseOfFramePointers;
    Vector<OperandUses> sinksOfFramePointers;
    Vector<uint32_t> offsets;
    Vector<SinkDependency> dependencies;

    uint32_t size() const { return sinks.size(); }

    std::span<const SinkDependency> dependenciesOf(uint32_t node) const LIFETIME_BOUND
    {
        return dependencies.subspan(offsets[node], offsets[node + 1] - offsets[node]);
    }
};

void solveSinkEquations(SinkEquations&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
