/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinkExceptions.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include <algorithm>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

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
