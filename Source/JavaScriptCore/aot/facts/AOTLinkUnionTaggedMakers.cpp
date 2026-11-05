/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTUnionTaggedMakers.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTLinkFlow.h"

namespace JSC { namespace AOT { namespace Typed {

void noteLayoutsMadeByTag(LinkFlow& flow, FunctionID reached)
{
    const TaggedMaker* maker = findTaggedMaker(flow.taggedMakers.span(), reached);
    if (!maker)
        return;
    for (const TaggedLayout& member : flow.program.taggedFamilies[maker->family].members)
        flow.noteAllocated(member.layout);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
