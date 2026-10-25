/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTVector.h"
#include <compare>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class FlowResults;
class ProgramEditor;

struct CalleeSetOfSite {
    AccessSiteID site;
    CalleeSetID callees;

    friend auto operator<=>(const CalleeSetOfSite&, const CalleeSetOfSite&) = default;
};

struct CalleeOfSite {
    AccessSiteID site;
    FunctionID callee;

    friend auto operator<=>(const CalleeOfSite&, const CalleeOfSite&) = default;
};

// What the call by value at each site may find, as flow left it. The lists are sorted.
struct CalleesOfSites {
    Vector<CalleeSetOfSite> calleeSets;
    Vector<CalleeOfSite> typedCallees;
    BitVector hadCallByValue;
    bool wasTaken { false };
};

CalleesOfSites collectCalleesOfSites(const Program&, BuildContext&);

// The closure of a function may be created though every call that finds it names the function by the time its caller is optimized.
void closeEntrypointsThatNothingEnters(ProgramEditor&, const FlowResults&, const CalleesOfSites&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
