/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTThreads.h"
#include <memory>
#include <span>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class FunctionAnalyses;
struct AnalysisContext;
struct OptimizationInputs;
struct OutliningSite;

// Looks at one function at a time, so that whoever has a function in the cache can have it looked at there.
class OutliningSiteFinder {
    WTF_MAKE_NONCOPYABLE(OutliningSiteFinder);
public:
    struct State;

    OutliningSiteFinder(const Program&, const OptimizationInputs&, BuildContext&);
    ~OutliningSiteFinder();

    // Once for a function, from any thread, while the caches of the program and an interning phase of its types are open.
    bool findIn(FunctionID);
    // With analyses of the function as it is now, made for contextOf(): whoever reads the function next may go on with them.
    bool findIn(FunctionID, FunctionAnalyses&);
    AnalysisContext contextOf(FunctionID) const;
    // By function, as many as the program had when the finder was made.
    std::span<const uint8_t> hasSites() const LIFETIME_BOUND;
    // Behind the last function. The sites stay where they were found, until discardSites().
    ParallelArray<OutliningSite*> chooseSites();
    void discardSites();

private:
    std::unique_ptr<State> m_state;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
