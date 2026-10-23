/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <memory>
#include <optional>
#include <span>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

struct OptimizationInputs;

struct OutliningSite {
    bool isRegion() const { return !!exitBlock; }

    FunctionID function;
    BlockID entryBlock;
    uint32_t firstIndex { 0 };
    uint32_t numberOfEntryValues { 0 };
    Vector<BlockID, 2> interiorBlocks;
    BlockID exitBlock;
    std::optional<uint32_t> exitArgument;
    ValueID root;
    bool rootIsUsed { false };
    bool canPayOff { false };
    uint32_t orderOfEntryBlock { 0 };
    Vector<ValueID, 6> liveIns;
    TypeID resultType;
    uint32_t codeBytes { 0 };
    uint32_t callBytes { 0 };
    unsigned hash { 0 };
};

inline std::span<const ValueID> entryValuesOf(const Function& function, const OutliningSite& site)
{
    return function.at(site.entryBlock).values.subspan(site.firstIndex, site.numberOfEntryValues);
}

// Looks at one function at a time, so that whoever has a function in the cache can have it looked at there.
class OutliningSiteFinder {
    WTF_MAKE_NONCOPYABLE(OutliningSiteFinder);
public:
    OutliningSiteFinder(const Program&, const OptimizationInputs&, BuildContext&);
    ~OutliningSiteFinder();

    // Once for a function, from any thread, while the caches of the program and an interning phase of its types are open.
    bool findIn(FunctionID);
    // By function, as many as the program had when the finder was made.
    std::span<const uint8_t> hasSites() const LIFETIME_BOUND;
    // Behind the last function.
    ParallelArray<OutliningSite> takeSites();

private:
    struct State;
    std::unique_ptr<State> m_state;
};

ParallelArray<OutliningSite> findOutliningSites(const Program&, const OptimizationInputs&, BuildContext&);

bool areEqualSites(const Program&, const OutliningSite&, const OutliningSite&);

uint32_t estimatedCallBytes(const OutliningSite&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
