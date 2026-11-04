/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Handlers: what a function throws flows to whoever may catch it, the handler around the call or what the caller throws in turn. It flows into unseen code too, whoever has thrown it: StateOfLinkReach::bringAlive() of AOTLinkReach.cpp lays thrown(id) > unseen() for every function that comes alive.
// At a throw: that is all of it. At a suspension: what a coroutine throws goes into its promise or to whoever steps it, which that edge covers.
// Withheld: the flow is the same, and LinkTables::catches is empty.

#include "config.h"
#include "AOTLinkExceptions.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include <algorithm>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

FlowNodeID nodeTakingWhatIsThrown(const LinkFlow& flow, FunctionID function, uint32_t thrownTo)
{
    return thrownTo == noSummaryNode ? flow.thrown(function) : flow.nodeOf(function, thrownTo);
}

TypeID LinkTables::caughtAt(FunctionID function, BlockID handler) const
{
    auto placeOf = [](const LinkedCatch& row) {
        return std::pair { row.function, row.handler };
    };
    auto found = std::ranges::lower_bound(catches, std::pair { function, handler }, { }, placeOf);
    return found != catches.end() && found->function == function && found->handler == handler ? found->caught : TypeID();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
