/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOpcode.h"
#include "AOTThreads.h"
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

inline constexpr OperandUses escapingUses {
    OperandUse::Retains,
    OperandUse::Returns,
    OperandUse::RetainsContents,
    OperandUse::PassesToUserCode,
};

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

enum class SinkTransform : uint8_t {
    Same,
    Argument,
    // An argument of a call that lies in a cycle: what the callee calls once is called once each time around.
    RepeatedArgument,
    Leaves,
    IfReturned,
    Lifetime,
    Contents,
    // The uses are those of an object, and the subject is a value that the object holds.
    Held,
};

OperandUses applyTransform(SinkTransform, OperandUses);

// A dependency without a condition names its node as the condition too, so the solver walks both alike.
struct SinkDependency {
    uint32_t node { 0 };
    uint32_t condition { 0 };
    SinkTransform transform { SinkTransform::Same };
};

// The large arrays are made by all threads: one thread would touch their memory for the first time alone.
struct SinkEquations {
    ParallelArray<OperandUses> base;
    ParallelArray<OperandUses> sinks;
    // A pointer into a frame may be in no cell, however soon that cell dies, and native code takes cells of the heap only.
    Vector<OperandUses> baseOfFramePointers;
    Vector<OperandUses> sinksOfFramePointers;
    Vector<uint32_t> offsets;
    ParallelArray<SinkDependency> dependencies;

    uint32_t size() const { return sinks.size(); }

    std::span<const SinkDependency> dependenciesOf(uint32_t node) const LIFETIME_BOUND
    {
        return dependencies.span().subspan(offsets[node], offsets[node + 1] - offsets[node]);
    }
};

// Solves for the nodes asked for and for all that they depend on. The sinks of another node stay what they were.
void solveSinkEquations(SinkEquations&, std::span<const uint32_t> nodesAskedFor, const ParallelLoops&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
