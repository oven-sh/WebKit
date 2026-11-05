/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTMechanisms.h"
#include "AOTTaggedLayouts.h"
#include "AOTVector.h"
#include <span>

namespace JSC { namespace AOT { namespace Typed {

class LinkFlow;
class Program;
class ThinSummaries;

struct TaggedMaker {
    uint32_t family { 0 };
    FunctionID constructor;
    uint32_t tagParameter { 0 };
};

Vector<TaggedMaker> findTaggedMakers(const Program&, const ThinSummaries&, Mechanisms);
const TaggedMaker* findTaggedMaker(std::span<const TaggedMaker> ascendingByConstructor, FunctionID constructor);
LayoutID layoutMadeWithTag(const TaggedFamily&, const LayoutTag&);
void noteLayoutsMadeByTag(LinkFlow&, FunctionID reached);
bool onlyTaggedMembersLieBelow(const Program&, LayoutID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
