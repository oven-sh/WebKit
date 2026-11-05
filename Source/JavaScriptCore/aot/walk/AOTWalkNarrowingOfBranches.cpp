/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

bool Walker::endBlockInAnotherWay(const BodyTerminator& terminator)
{
    return terminator.kind() == TerminatorKind::Branch && endBlockWithSwitchOverAlternatives(terminator);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
